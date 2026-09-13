/* cas_range_test - the bounded CAS range reader against the full-read path.
 *
 *   cas_range_test [scratch_dir]
 *
 * Synthetic only; needs no install and does not load Oodle. Every case builds
 * a CAS reference on disk from stored (codec 0) blocks, then asks the same
 * question two ways: cas_read + partition_guid_from_bytes (the original full
 * path) and CasRangeReader + partition_guid_from_reader (the bounded path).
 * The two must agree on every case, including the malformed ones, except the
 * one DOCUMENTED divergence (a bad block past every requested range), which is
 * asserted as a divergence so it cannot silently change either.
 *
 * Controls: a perturbed GUID must change both answers; a record read with the
 * wrong expected GUID must mismatch; Oodle-coded blocks with no Oodle loaded
 * must fail both ways; random range reads must equal the full bytes.
 */
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "cas.h"
#include "source.h"

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what) {
    if (ok) ++g_pass; else { ++g_fail; std::printf("FAIL: %s\n", what); }
}

std::filesystem::path g_dir;
int g_file_no = 0;

void put_le32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void put_be32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 3; i >= 0; --i) o.push_back(uint8_t(v >> (8 * i)));
}
void chunk(std::vector<uint8_t>& o, const char* tag, const std::vector<uint8_t>& body, bool pad) {
    o.insert(o.end(), tag, tag + 4);
    put_le32(o, (uint32_t)body.size());
    o.insert(o.end(), body.begin(), body.end());
    if (pad && (body.size() & 1)) o.push_back(0);
}
std::vector<uint8_t> filler(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(seed + i * 31);
    return v;
}
std::vector<uint8_t> guid_bytes(uint8_t seed) {
    std::vector<uint8_t> g(16);
    for (int i = 0; i < 16; ++i) g[i] = uint8_t(seed * 7 + i);
    return g;
}

// RIFF record: header + the given chunks (already encoded).
std::vector<uint8_t> riff(const std::vector<uint8_t>& chunks) {
    std::vector<uint8_t> o = {'R', 'I', 'F', 'F'};
    put_le32(o, (uint32_t)(chunks.size() + 4));
    o.insert(o.end(), {'E', 'B', 'X', 'D'});
    o.insert(o.end(), chunks.begin(), chunks.end());
    return o;
}

// Split `raw` into blocks of `bs` decompressed bytes, all stored.
std::vector<uint8_t> blocks(const std::vector<uint8_t>& raw, size_t bs, uint8_t codec = 0) {
    std::vector<uint8_t> o;
    for (size_t p = 0; p < raw.size(); p += bs) {
        const uint32_t n = (uint32_t)std::min(bs, raw.size() - p);
        put_be32(o, n);
        put_be32(o, (uint32_t(codec) << 24) | (7u << 20) | n);
        o.insert(o.end(), raw.begin() + p, raw.begin() + p + n);
    }
    return o;
}

std::string write_file(const std::vector<uint8_t>& bytes, size_t prefix = 13) {
    // A fresh name per case: CAS handles are cached per thread by path.
    const std::filesystem::path p = g_dir / ("case_" + std::to_string(g_file_no++) + ".cas");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    std::vector<uint8_t> pre(prefix, 0xEE);   // reference not at offset 0
    std::fwrite(pre.data(), 1, pre.size(), f);
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return p.string();
}

struct Both {
    std::string full, bounded;
    bf6::CasReadStats fs, bs;
};

Both both(const std::string& path, int64_t off, int64_t size) {
    Both r;
    std::string e;
    std::vector<uint8_t> bytes = bf6::cas_read(path, off, size, false, e, &r.fs);
    r.full = bytes.empty() ? std::string() : bf6::partition_guid_from_bytes(bytes);
    {
        bf6::CasRangeReader rd;
        if (rd.open(path, off, size, false, e, &r.bs)) r.bounded = bf6::partition_guid_from_reader(rd);
    }
    return r;
}

Both both_bytes(const std::vector<uint8_t>& cas, size_t declared_extra = 0, size_t write_len = SIZE_MAX) {
    std::vector<uint8_t> w(cas.begin(), cas.begin() + std::min(write_len, cas.size()));
    const std::string path = write_file(w);
    return both(path, 13, (int64_t)(cas.size() + declared_extra));
}

std::string fmt_guid(const std::vector<uint8_t>& g) {
    char b[40];
    std::snprintf(b, sizeof(b), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)(g[0] | (g[1] << 8) | (g[2] << 16) | ((unsigned)g[3] << 24)),
        (unsigned)(g[4] | (g[5] << 8)), (unsigned)(g[6] | (g[7] << 8)),
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}

void report(const char* name, const Both& r) {
    std::printf("  %-38s full=%-36s bounded=%-36s bytes %llu/%llu blocks skipped %llu\n",
        name, r.full.empty() ? "-" : r.full.c_str(), r.bounded.empty() ? "-" : r.bounded.c_str(),
        (unsigned long long)r.fs.file_bytes, (unsigned long long)r.bs.file_bytes,
        (unsigned long long)r.bs.blocks_skipped);
}

}  // namespace

int main(int argc, char** argv) {
    g_dir = argc > 1 ? std::filesystem::path(argv[1])
                     : std::filesystem::temp_directory_path() / "bf6_cas_range_test";
    std::error_code ec;
    // A supplied path is a parent, never a disposable directory. Create a
    // unique child and leave its evidence intact; do not recursively delete.
    g_dir /= "cas-range-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    if (!std::filesystem::create_directories(g_dir, ec) || ec) {
        std::fprintf(stderr, "Cannot create test scratch directory: %s\n", g_dir.string().c_str());
        return 2;
    }

    const std::vector<uint8_t> g1 = guid_bytes(1), g2 = guid_bytes(2);
    const std::string G1 = fmt_guid(g1);

    // 1. Typical layout: big EBXD, then EFIX, then a trailing chunk; multi-block.
    {
        std::vector<uint8_t> c;
        // Larger than the reader's 16 KiB head prefetch, so skipping is visible.
        chunk(c, "EBXD", filler(100001, 3), true);
        std::vector<uint8_t> efix = g1; auto tail = filler(40, 9); efix.insert(efix.end(), tail.begin(), tail.end());
        chunk(c, "EFIX", efix, true);
        chunk(c, "EBXX", filler(100, 5), true);
        Both r = both_bytes(blocks(riff(c), 4096));
        report("multi-block, EFIX after large EBXD", r);
        check(r.full == G1 && r.bounded == G1, "multi-block GUID agrees");
        check(r.bs.file_bytes < r.fs.file_bytes / 4, "bounded read far fewer bytes");
        check(r.bs.blocks_skipped > 10, "middle blocks skipped");
        check(r.bs.blocks_decoded == 0 && r.fs.blocks_decoded == 0, "stored blocks never decode");
    }
    // 2. Single stored block.
    {
        std::vector<uint8_t> c; chunk(c, "EBXD", filler(33, 1), true); chunk(c, "EFIX", g1, true);
        Both r = both_bytes(blocks(riff(c), 1 << 20));
        report("single block", r);
        check(r.full == G1 && r.bounded == G1, "single block GUID agrees");
    }
    // 3. EFIX FIRST (no constant offset assumed) and EFIX after two chunks.
    {
        std::vector<uint8_t> c; chunk(c, "EFIX", g2, true); chunk(c, "EBXD", filler(700, 4), true);
        Both r = both_bytes(blocks(riff(c), 64));
        report("EFIX first", r);
        check(r.full == fmt_guid(g2) && r.bounded == r.full, "EFIX-first agrees");
        std::vector<uint8_t> d; chunk(d, "AAAA", filler(3, 1), true); chunk(d, "BBBB", filler(900, 2), true);
        chunk(d, "EFIX", g1, true);
        Both r2 = both_bytes(blocks(riff(d), 100));
        report("EFIX third, odd first chunk", r2);
        check(r2.full == G1 && r2.bounded == G1, "EFIX-third with padding agrees");
    }
    // 4. Padding control: odd chunk WITHOUT its pad byte. The walk lands one
    //    byte late; both must miss identically rather than one finding it.
    {
        std::vector<uint8_t> c; chunk(c, "EBXD", filler(301, 6), false); chunk(c, "EFIX", g1, true);
        Both r = both_bytes(blocks(riff(c), 50));
        report("CONTROL odd chunk, pad missing", r);
        check(r.full.empty() && r.bounded.empty(), "missing pad: both miss");
    }
    // 5. Chunk header and GUID straddling block boundaries.
    {
        for (size_t bs = 5; bs <= 29; bs += 3) {
            std::vector<uint8_t> c; chunk(c, "EBXD", filler(57, 7), true); chunk(c, "EFIX", g1, true);
            Both r = both_bytes(blocks(riff(c), bs));
            check(r.full == G1 && r.bounded == G1, "straddling boundary agrees");
        }
        std::printf("  straddle sweep block sizes 5..29 done\n");
    }
    // 6. Truncations: archive shorter than the declared size, single and multi.
    {
        std::vector<uint8_t> c; chunk(c, "EBXD", filler(400, 1), true); chunk(c, "EFIX", g1, true);
        const std::vector<uint8_t> one = blocks(riff(c), 1 << 20);
        Both r = both_bytes(one, 0, one.size() - 5);
        report("CONTROL truncated single block", r);
        check(r.full.empty() && r.bounded.empty(), "truncated single: both fail");
        const std::vector<uint8_t> many = blocks(riff(c), 64);
        Both r2 = both_bytes(many, 0, many.size() - 20);
        report("CONTROL truncated multi (EFIX lost)", r2);
        check(r2.full.empty() && r2.bounded.empty(), "truncated multi: both miss");
        Both r3 = both_bytes(many, 0, 7);
        check(r3.full.empty() && r3.bounded.empty(), "shorter than a header: both fail");
        Both r4 = both_bytes(std::vector<uint8_t>(), 0, 0);
        check(r4.full.empty() && r4.bounded.empty(), "empty reference: both fail");
        const std::string p = write_file(many);
        Both r5 = both(p, 1LL << 40, 100);
        check(r5.full.empty() && r5.bounded.empty(), "offset past archive end: both fail");
        Both r6 = both(p, -4, 100);
        check(r6.full.empty() && r6.bounded.empty(), "negative offset: both fail");
    }
    // 7. Structural failures.
    {
        std::vector<uint8_t> c; chunk(c, "EBXD", filler(90, 1), true); chunk(c, "EFIX", g1, true);
        std::vector<uint8_t> rec = riff(c);
        // One block plus trailing garbage: not single (csize != size-8), <2 parts.
        std::vector<uint8_t> one = blocks(rec, 1 << 20); one.insert(one.end(), 12, 0);
        Both r = both_bytes(one);
        check(r.full.empty() && r.bounded.empty(), "one part + garbage: both fail");
        // Guard broken on block 0.
        std::vector<uint8_t> bad = blocks(rec, 32); bad[4] = 0x00; bad[5] &= 0x0F;
        Both r2 = both_bytes(bad);
        check(r2.full.empty() && r2.bounded.empty(), "no guard: both fail");
        // Guard broken on block 2: record is blocks 0..1 only, EFIX lost.
        std::vector<uint8_t> brk = blocks(rec, 32); brk[2 * 40 + 5] &= 0x0F;
        Both r3 = both_bytes(brk);
        check(r3.full.empty() && r3.bounded.empty(), "guard break before EFIX: both miss");
        // Stored size mismatch on block 1 (dsize != csize).
        std::vector<uint8_t> mm = blocks(rec, 32); mm[40 + 3] ^= 1;
        Both r4 = both_bytes(mm);
        check(r4.full.empty() && r4.bounded.empty(), "stored size mismatch: both fail");
        // Unknown codec on block 0.
        std::vector<uint8_t> cc = blocks(rec, 32); cc[4] = 0x42;
        Both r5 = both_bytes(cc);
        check(r5.full.empty() && r5.bounded.empty(), "unknown codec: both fail");
        // Oodle codec, Oodle not loaded: every decode fails.
        Both r6 = both_bytes(blocks(rec, 32, 0x15));
        check(r6.full.empty() && r6.bounded.empty(), "oodle without oodle: both fail");
        // No RIFF magic / huge chunk size / GUID cut at the record end.
        std::vector<uint8_t> nr = rec; nr[0] = 'X';
        Both r7 = both_bytes(blocks(nr, 32));
        check(r7.full.empty() && r7.bounded.empty(), "no RIFF: both miss");
        std::vector<uint8_t> hc; chunk(hc, "EBXD", filler(10, 1), true);
        hc[4] = 0xF0; hc[5] = 0xFF; hc[6] = 0xFF; hc[7] = 0xFF;
        chunk(hc, "EFIX", g1, true);
        Both r8 = both_bytes(blocks(riff(hc), 32));
        check(r8.full.empty() && r8.bounded.empty(), "huge chunk size: both miss, no hang");
        std::vector<uint8_t> cut; chunk(cut, "EBXD", filler(10, 1), true);
        cut.insert(cut.end(), {'E', 'F', 'I', 'X', 16, 0, 0, 0}); cut.insert(cut.end(), g1.begin(), g1.begin() + 9);
        Both r9 = both_bytes(blocks(riff(cut), 16));
        check(r9.full.empty() && r9.bounded.empty(), "GUID cut short: both miss");
    }
    // 8. DOCUMENTED DIVERGENCE: a bad codec in a block past EFIX. The full read
    //    rejects the whole record; the bounded read never reaches that block
    //    until the walk needs it. EFIX here sits in block 0.
    {
        std::vector<uint8_t> c; chunk(c, "EFIX", g1, true); chunk(c, "EBXD", filler(200, 1), true);
        std::vector<uint8_t> cas = blocks(riff(c), 64);
        cas[3 * (8 + 64) + 4] = 0x42;  // block 3's codec
        Both r = both_bytes(cas);
        report("KNOWN LIMIT bad block past EFIX", r);
        check(r.full.empty() && r.bounded == G1, "divergence is exactly the documented one");
    }
    // 9. Controls on the answer itself.
    {
        std::vector<uint8_t> c; chunk(c, "EBXD", filler(600, 2), true); chunk(c, "EFIX", g1, true);
        std::vector<uint8_t> rec = riff(c);
        Both a = both_bytes(blocks(rec, 64));
        rec[rec.size() - 1] ^= 0x5A;   // perturb the last GUID byte
        Both b = both_bytes(blocks(rec, 64));
        report("CONTROL perturbed GUID", b);
        check(b.full == b.bounded && b.full != a.full && !b.full.empty(), "perturbed GUID changes both");
        check(a.bounded != fmt_guid(g2), "wrong expected GUID does not match");
    }
    // 10. Random range reads equal the full bytes; out-of-range refused.
    {
        std::vector<uint8_t> rec = filler(10000, 11);
        std::vector<uint8_t> cas = blocks(rec, 97);
        const std::string p = write_file(cas);
        std::string e;
        std::vector<uint8_t> full = bf6::cas_read(p, 13, (int64_t)cas.size(), false, e);
        check(full == rec, "full read reproduces the record");
        bf6::CasReadStats st;
        bf6::CasRangeReader rd;
        check(rd.open(p, 13, (int64_t)cas.size(), false, e, &st), "range reader opens");
        std::mt19937 rng(1234);
        bool all = true;
        for (int i = 0; i < 2000; ++i) {
            const size_t at = rng() % rec.size();
            const size_t n = rng() % std::min<size_t>(400, rec.size() - at + 1);
            std::vector<uint8_t> got(n);
            if (!rd.read(at, n, got.data(), e) || std::memcmp(got.data(), rec.data() + at, n) != 0) all = false;
        }
        check(all, "2000 random ranges equal full bytes");
        uint8_t x[4];
        check(!rd.read(rec.size() - 2, 4, x, e), "range past end refused");
        check(!rd.read(UINT64_MAX - 1, 4, x, e), "overflowing range refused");
        uint64_t total = 0;
        check(rd.total_size(total, e) && total == rec.size(), "total size matches");
        // Raw (no guard) reference with allow_raw.
        const std::string rp = write_file(rec);
        std::vector<uint8_t> rawfull = bf6::cas_read(rp, 13, (int64_t)rec.size(), true, e);
        bf6::CasRangeReader rr;
        std::vector<uint8_t> mid(300);
        check(rr.open(rp, 13, (int64_t)rec.size(), true, e) && rr.read(5000, 300, mid.data(), e) &&
              rawfull == rec && std::memcmp(mid.data(), rec.data() + 5000, 300) == 0, "raw mode agrees");
    }
    // 11. The A/B toggle.
    {
#ifdef _MSC_VER
        _putenv_s("BF6_PARTITION_INDEX_FULL_READ", "1");
#else
        setenv("BF6_PARTITION_INDEX_FULL_READ", "1", 1);
#endif
        bf6::Source s;
        check(s.partition_index_full_read(), "env selects full read");
        s.set_partition_index_full_read(false);
        check(!s.partition_index_full_read(), "setter overrides env");
#ifdef _MSC_VER
        _putenv_s("BF6_PARTITION_INDEX_FULL_READ", "0");
#else
        setenv("BF6_PARTITION_INDEX_FULL_READ", "0", 1);
#endif
        bf6::Source s2;
        check(!s2.partition_index_full_read(), "env 0 selects bounded");
    }

    std::printf("cas_range_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
