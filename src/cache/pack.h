// Pack files: many named records in one data file plus one index, instead of
// tens of thousands of small files. See docs/PRECACHE.md.
//
//   <name>.pack   records back to back
//   <name>.idx    "BF6PIDX1" | u32 count | per record: u16 name_len, name,
//                 u64 offset, u64 size, u64 fp_a, u64 fp_b | u64 fp of index
//
// A pack is written once and published by renaming both files; readers never see
// a half-written pack. Record payloads are verified against their fingerprint on
// read.
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf6::cache {

namespace fs = std::filesystem;

struct Digest {
    std::uint64_t a = 0, b = 0;
    bool operator==(const Digest& o) const { return a == o.a && b == o.b; }
    bool operator!=(const Digest& o) const { return !(*this == o); }
    std::string hex() const;
};

Digest digest_of(const char* data, std::size_t size);

class PackWriter {
public:
    // Starts a new pack at <dir>/<name>.pack|.idx (written to temporaries).
    bool begin(const fs::path& dir, const std::string& name, std::string& err);
    // Appends one record. Names are unique within a pack.
    bool add(const std::string& name, const char* data, std::size_t size, std::string& err);
    bool add(const std::string& name, const std::string& bytes, std::string& err)
    { return add(name, bytes.data(), bytes.size(), err); }
    // Flushes and publishes both files. Returns the pack's total payload bytes.
    bool finish(std::uint64_t& bytes, Digest& pack_digest, std::string& err);
    void abandon();
    // Reads back a record added to this unfinished pack.
    bool read_back(const std::string& name, std::string& out, std::string& err);
    ~PackWriter() { abandon(); }

private:
    struct Row { std::string name; std::uint64_t offset, size; Digest digest; };
    fs::path dir_, data_tmp_, index_tmp_;
    std::string name_;
    std::ofstream out_;
    std::uint64_t cursor_ = 0;
    std::vector<Row> rows_;
    std::unordered_map<std::string, std::size_t> seen_;
    bool open_ = false;
};

class PackReader {
public:
    bool open(const fs::path& dir, const std::string& name, std::string& err);
    bool has(const std::string& name) const { return index_.count(name) != 0; }
    std::size_t size() const { return index_.size(); }
    std::vector<std::string> names() const;
    // Reads and verifies one record. Thread-safe.
    bool read(const std::string& name, std::string& out, std::string& err) const;

private:
    struct Row { std::uint64_t offset, size; Digest digest; };
    fs::path data_;
    std::unordered_map<std::string, Row> index_;
    mutable std::mutex mutex_;
    mutable std::ifstream in_;
};

// Shared content-addressed store: each unique payload is written once no matter
// how many maps use it. Appends to numbered segment packs; thread-safe.
class ContentStore {
public:
    static constexpr std::uint64_t kSegmentBytes = 512ull * 1024 * 1024;

    bool open(const fs::path& dir, std::string& err);
    // Stores bytes under key if absent. `added` reports whether it was new.
    bool put(const std::string& key, const std::string& bytes, bool& added, std::string& err);
    bool contains(const std::string& key) const;
    bool get(const std::string& key, std::string& out, std::string& err) const;
    // Publishes the open segment. Call once at the end of a build.
    bool flush(std::string& err);
    std::uint64_t bytes_written() const { return written_; }

private:
    bool roll(std::string& err);
    fs::path dir_;
    mutable std::mutex mutex_;
    std::map<std::string, std::uint32_t> where_;   // key -> segment number
    std::vector<std::unique_ptr<PackReader>> readers_;
    PackWriter writer_;
    std::uint32_t segment_ = 0;
    std::uint64_t segment_bytes_ = 0;
    std::uint64_t written_ = 0;
    bool writing_ = false;
};

}  // namespace bf6::cache
