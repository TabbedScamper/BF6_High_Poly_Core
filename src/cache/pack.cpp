#include "pack.h"

#include "cache_store.h"
#include "../../include/bf6_cache_identity.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <system_error>

namespace bf6::cache {

namespace {

const char kMagic[8] = { 'B','F','6','P','I','D','X','1' };

void put_u16(std::string& s, std::uint16_t v) { s.push_back((char)(v & 255)); s.push_back((char)(v >> 8)); }
void put_u32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back((char)((v >> (8 * i)) & 255)); }
void put_u64(std::string& s, std::uint64_t v) { for (int i = 0; i < 8; ++i) s.push_back((char)((v >> (8 * i)) & 255)); }

bool get_u16(const std::string& s, std::size_t& at, std::uint16_t& v)
{ if (at + 2 > s.size()) return false; v = (std::uint16_t)((unsigned char)s[at] | ((unsigned char)s[at + 1] << 8)); at += 2; return true; }
bool get_u32(const std::string& s, std::size_t& at, std::uint32_t& v)
{ if (at + 4 > s.size()) return false; v = 0; for (int i = 0; i < 4; ++i) v |= (std::uint32_t)(unsigned char)s[at + i] << (8 * i); at += 4; return true; }
bool get_u64(const std::string& s, std::size_t& at, std::uint64_t& v)
{ if (at + 8 > s.size()) return false; v = 0; for (int i = 0; i < 8; ++i) v |= (std::uint64_t)(unsigned char)s[at + i] << (8 * i); at += 8; return true; }

}  // namespace

std::string Digest::hex() const
{
    static const char* d = "0123456789abcdef";
    std::string s(32, '0');
    std::uint64_t x = a, y = b;
    for (int i = 15; i >= 0; --i) { s[(size_t)i] = d[x & 15]; x >>= 4; }
    for (int i = 31; i >= 16; --i) { s[(size_t)i] = d[y & 15]; y >>= 4; }
    return s;
}

Digest digest_of(const char* data, std::size_t size)
{
    bf6_cache::detail::Hasher h;
    h.bytes((const unsigned char*)data, size);
    const auto f = h.done();
    return { f.a, f.b };
}

// ---- writer ------------------------------------------------------------------

bool PackWriter::begin(const fs::path& dir, const std::string& name, std::string& err)
{
    abandon();
    if (name.empty() || name.find_first_of("/\\:") != std::string::npos) { err = "invalid pack name"; return false; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { err = "cannot create " + dir.u8string() + ": " + ec.message(); return false; }
    dir_ = dir; name_ = name;
    data_tmp_ = dir / (name + ".pack.writing");
    index_tmp_ = dir / (name + ".idx.writing");
    out_.clear();
    errno = 0;
    out_.open(data_tmp_, std::ios::binary | std::ios::trunc);
    if (!out_) {
        const int code = errno;
        err = "cannot write " + data_tmp_.u8string() + ": " + std::strerror(code);
        return false;
    }
    cursor_ = 0; rows_.clear(); seen_.clear();
    open_ = true;
    return true;
}

bool PackWriter::add(const std::string& name, const char* data, std::size_t size, std::string& err)
{
    if (!open_) { err = "pack not open"; return false; }
    if (name.empty() || name.size() > 0xFFFF) { err = "invalid record name"; return false; }
    if (seen_.count(name)) { err = "duplicate record " + name; return false; }
    out_.write(data, (std::streamsize)size);
    if (!out_) { err = "write failed in " + data_tmp_.u8string(); return false; }
    seen_.emplace(name, rows_.size());
    rows_.push_back({ name, cursor_, (std::uint64_t)size, digest_of(data, size) });
    cursor_ += size;
    return true;
}

bool PackWriter::finish(std::uint64_t& bytes, Digest& pack_digest, std::string& err)
{
    if (!open_) { err = "pack not open"; return false; }
    out_.flush();
    const bool good = (bool)out_;
    out_.close();
    if (!good) { err = "flush failed " + data_tmp_.u8string(); abandon(); return false; }
    std::string idx(kMagic, 8);
    put_u32(idx, (std::uint32_t)rows_.size());
    for (const Row& r : rows_) {
        put_u16(idx, (std::uint16_t)r.name.size());
        idx += r.name;
        put_u64(idx, r.offset); put_u64(idx, r.size); put_u64(idx, r.digest.a); put_u64(idx, r.digest.b);
    }
    pack_digest = digest_of(idx.data(), idx.size());
    put_u64(idx, pack_digest.a);
    {
        std::ofstream io(index_tmp_, std::ios::binary | std::ios::trunc);
        io.write(idx.data(), (std::streamsize)idx.size());
        io.flush();
        if (!io) { err = "cannot write index " + index_tmp_.u8string(); abandon(); return false; }
    }
    std::error_code ec;
    const fs::path data = dir_ / (name_ + ".pack"), index = dir_ / (name_ + ".idx");
    // Data first, index last: a pack without its index is ignored by readers.
    fs::remove(index, ec);
    fs::rename(data_tmp_, data, ec);
    if (!ec) fs::rename(index_tmp_, index, ec);
    if (ec) { err = "cannot publish pack " + name_ + ": " + ec.message(); abandon(); return false; }
    bytes = cursor_;
    open_ = false;
    return true;
}

bool PackWriter::read_back(const std::string& name, std::string& out, std::string& err)
{
    const auto it = seen_.find(name);
    if (!open_ || it == seen_.end()) { err = "no pending record " + name; return false; }
    out_.flush();
    const Row& r = rows_[it->second];
    std::ifstream in(data_tmp_, std::ios::binary);
    in.seekg((std::streamoff)r.offset);
    out.resize((std::size_t)r.size);
    in.read(out.data(), (std::streamsize)out.size());
    if (!in || digest_of(out.data(), out.size()) != r.digest) { err = "pending record unreadable " + name; return false; }
    return true;
}

void PackWriter::abandon()
{
    if (out_.is_open()) out_.close();
    if (open_) {
        std::error_code ec;
        fs::remove(data_tmp_, ec);
        fs::remove(index_tmp_, ec);
    }
    open_ = false;
}

// ---- reader ------------------------------------------------------------------

bool PackReader::open(const fs::path& dir, const std::string& name, std::string& err)
{
    std::string idx;
    if (!read_file(dir / (name + ".idx"), idx)) { err = "missing index " + name; return false; }
    if (idx.size() < 20 || std::memcmp(idx.data(), kMagic, 8) != 0) { err = "bad index header " + name; return false; }
    const Digest whole = digest_of(idx.data(), idx.size() - 8);
    std::size_t tail = idx.size() - 8;
    std::uint64_t stored = 0;
    if (!get_u64(idx, tail, stored) || stored != whole.a) { err = "index checksum mismatch " + name; return false; }
    std::size_t at = 8;
    std::uint32_t count = 0;
    if (!get_u32(idx, at, count)) { err = "truncated index " + name; return false; }
    index_.clear();
    index_.reserve(count);
    const std::size_t end = idx.size() - 8;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t len = 0;
        if (!get_u16(idx, at, len) || at + len > end) { err = "truncated index " + name; return false; }
        std::string rec = idx.substr(at, len);
        at += len;
        Row r{};
        if (!get_u64(idx, at, r.offset) || !get_u64(idx, at, r.size) ||
            !get_u64(idx, at, r.digest.a) || !get_u64(idx, at, r.digest.b) || at > end) {
            err = "truncated index " + name; return false;
        }
        index_.emplace(std::move(rec), r);
    }
    data_ = dir / (name + ".pack");
    std::error_code ec;
    const auto data_size = fs::file_size(data_, ec);
    if (ec) { err = "missing data " + name; return false; }
    for (const auto& kv : index_)
        if (kv.second.offset + kv.second.size > data_size) { err = "index exceeds data " + name; return false; }
    in_.open(data_, std::ios::binary);
    if (!in_) { err = "cannot open data " + name; return false; }
    return true;
}

std::vector<std::string> PackReader::names() const
{
    std::vector<std::string> out;
    out.reserve(index_.size());
    for (const auto& kv : index_) out.push_back(kv.first);
    return out;
}

bool PackReader::read(const std::string& name, std::string& out, std::string& err) const
{
    const auto it = index_.find(name);
    if (it == index_.end()) { err = "no record " + name; return false; }
    out.resize((std::size_t)it->second.size);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_.clear();
        in_.seekg((std::streamoff)it->second.offset);
        in_.read(out.data(), (std::streamsize)out.size());
        if (!in_) { err = "short read " + name; return false; }
    }
    if (digest_of(out.data(), out.size()) != it->second.digest) { err = "record checksum mismatch " + name; return false; }
    return true;
}

// ---- shared content store ---------------------------------------------------------

bool ContentStore::open(const fs::path& dir, std::string& err)
{
    std::lock_guard<std::mutex> lock(mutex_);
    dir_ = dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { err = "cannot create store: " + ec.message(); return false; }
    where_.clear(); readers_.clear();
    segment_ = 0;
    for (std::uint32_t n = 0;; ++n) {
        const std::string name = "segment-" + std::to_string(n);
        if (!fs::exists(dir / (name + ".idx"), ec)) break;
        auto reader = std::make_unique<PackReader>();
        if (!reader->open(dir, name, err)) return false;
        for (const auto& key : reader->names()) where_.emplace(key, n);
        readers_.push_back(std::move(reader));
        segment_ = n + 1;
    }
    return true;
}

bool ContentStore::roll(std::string& err)
{
    std::uint64_t bytes = 0; Digest d;
    if (writing_) {
        if (!writer_.finish(bytes, d, err)) return false;
        auto reader = std::make_unique<PackReader>();
        if (!reader->open(dir_, "segment-" + std::to_string(segment_), err)) return false;
        readers_.push_back(std::move(reader));
        ++segment_;
        writing_ = false;
    }
    return true;
}

bool ContentStore::put(const std::string& key, const std::string& bytes, bool& added, std::string& err)
{
    std::lock_guard<std::mutex> lock(mutex_);
    added = false;
    if (where_.count(key)) return true;
    if (!writing_) {
        if (!writer_.begin(dir_, "segment-" + std::to_string(segment_), err)) return false;
        writing_ = true;
        segment_bytes_ = 0;
    }
    if (!writer_.add(key, bytes, err)) return false;
    where_.emplace(key, segment_);
    segment_bytes_ += bytes.size();
    written_ += bytes.size();
    added = true;
    if (segment_bytes_ >= kSegmentBytes) return roll(err);
    return true;
}

bool ContentStore::contains(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return where_.count(key) != 0;
}

bool ContentStore::get(const std::string& key, std::string& out, std::string& err) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = where_.find(key);
    if (it == where_.end()) { err = "not in store " + key; return false; }
    if (writing_ && it->second == segment_) return const_cast<PackWriter&>(writer_).read_back(key, out, err);
    if (it->second >= readers_.size()) { err = "segment missing for " + key; return false; }
    return readers_[it->second]->read(key, out, err);
}

bool ContentStore::flush(std::string& err)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return roll(err);
}

}  // namespace bf6::cache
