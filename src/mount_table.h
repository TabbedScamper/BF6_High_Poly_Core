/* libbf6 internal - the mount's name tables, readable in place from a mapped
 * snapshot.
 *
 * WHY THIS EXISTS. Mounting a level builds about 1.4 million name -> location
 * entries, and doing that from the archives costs seconds and several hundred
 * megabytes of small allocations every time a process opens a level. The same
 * mount on the same installation always produces the same tables, so the result
 * is written once as fixed-size records over a name blob with an open-addressed
 * hash index, and later processes MAP that file and look names up in it without
 * building anything. Nothing is deserialised per entry.
 *
 * A table is a stack of read-only mapped LAYERS followed by one ordinary map
 * (the overlay) for anything mounted after them. Layers are searched first, in
 * order, which keeps the mount's first-mount-wins rule: an overlay entry is only
 * ever added for a name no layer carries.
 *
 * Iteration yields items with a std::string `first` and the entry as `second`,
 * so `for (const auto& kv : table)`, `find`, `count`, `end` and `size` read the
 * same as they did over std::unordered_map / std::map.
 */
#ifndef LIBBF6_MOUNT_TABLE_H
#define LIBBF6_MOUNT_TABLE_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace bf6 {

// A read-only view of a whole file, kept alive by every table layer using it.
class MappedFile {
public:
    static std::shared_ptr<MappedFile> open(const std::string& path);
    ~MappedFile();
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

private:
    MappedFile() = default;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    void* file_ = nullptr;
    void* mapping_ = nullptr;
};

inline uint64_t mount_name_hash(const char* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ (uint8_t)p[i]) * 1099511628211ull;
    return h;
}

template <class E, bool Ordered>
class MountTable {
public:
    static constexpr uint32_t kNoBundle = 0xFFFFFFFFu;

    // On-disk record. E is a small trivially copyable struct.
    struct Rec {
        uint32_t name_off;
        uint32_t name_len;
        uint32_t bundle;
        uint32_t reserved;
        E        e;
    };
    struct Layer {
        const Rec*      recs = nullptr;
        uint32_t        count = 0;
        const uint32_t* slots = nullptr;   // record index + 1, 0 = empty
        uint32_t        mask = 0;          // slot count - 1 (a power of two)
        const char*     names = nullptr;
        size_t          names_size = 0;
    };
    struct Stored { E e; uint32_t bundle = kNoBundle; };
    using Overlay = typename std::conditional<Ordered,
        std::map<std::string, Stored>,
        std::unordered_map<std::string, Stored>>::type;

    // What iteration yields. Both references are STABLE for the table's
    // lifetime, exactly as a map's key and value were: callers keep
    // `kv.first.c_str()` and `&kv.second` after the loop moves on. A mapped
    // record's name is made a std::string once, on first use, and kept.
    struct Ref { const std::string& first; const E& second; };
    struct Arrow { Ref r; const Ref* operator->() const { return &r; } };

    class const_iterator {
    public:
        using value_type = Ref;
        using reference = Ref;
        using pointer = Arrow;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;

        const_iterator() = default;
        Ref operator*() const
        {
            if (layer_ < t_->layers_.size())
                return Ref{t_->name_at(layer_, index_), t_->layers_[layer_].recs[index_].e};
            return Ref{it_->first, it_->second.e};
        }
        Arrow operator->() const { return Arrow{**this}; }
        const_iterator& operator++()
        {
            if (layer_ < t_->layers_.size()) { ++index_; settle(); }
            else ++it_;
            return *this;
        }
        bool operator==(const const_iterator& o) const
        {
            if (layer_ != o.layer_) return false;
            if (layer_ < t_->layers_.size()) return index_ == o.index_;
            return it_ == o.it_;
        }
        bool operator!=(const const_iterator& o) const { return !(*this == o); }

    private:
        friend class MountTable;
        const_iterator(const MountTable* t, size_t layer, uint32_t index,
                       typename Overlay::const_iterator it)
            : t_(t), layer_(layer), index_(index), it_(it) { settle(); }
        void settle()
        {
            while (layer_ < t_->layers_.size() && index_ >= t_->layers_[layer_].count)
            { ++layer_; index_ = 0; }
        }
        const MountTable* t_ = nullptr;
        size_t layer_ = 0;
        uint32_t index_ = 0;
        typename Overlay::const_iterator it_;
    };

    MountTable() = default;
    MountTable(const MountTable&) = delete;
    MountTable& operator=(const MountTable&) = delete;
    ~MountTable() { drop_names(); }

    const_iterator begin() const { return const_iterator(this, 0, 0, overlay_.begin()); }
    const_iterator end() const { return const_iterator(this, layers_.size(), 0, overlay_.end()); }

    size_t size() const { return layered_ + overlay_.size(); }
    bool empty() const { return size() == 0; }

    // The entry for a name, or null; `bundle` receives its bundle index.
    const E* lookup(const std::string& name, uint32_t* bundle = nullptr) const
    {
        if (!layers_.empty()) {
            const uint64_t h = mount_name_hash(name.data(), name.size());
            for (const Layer& l : layers_) {
                if (const Rec* r = probe(l, name, h)) {
                    if (bundle) *bundle = r->bundle;
                    return &r->e;
                }
            }
        }
        auto it = overlay_.find(name);
        if (it == overlay_.end()) return nullptr;
        if (bundle) *bundle = it->second.bundle;
        return &it->second.e;
    }

    const_iterator find(const std::string& name) const
    {
        if (!layers_.empty()) {
            const uint64_t h = mount_name_hash(name.data(), name.size());
            for (size_t i = 0; i < layers_.size(); ++i)
                if (const Rec* r = probe(layers_[i], name, h))
                    return const_iterator(this, i, (uint32_t)(r - layers_[i].recs), overlay_.begin());
        }
        auto it = overlay_.find(name);
        if (it == overlay_.end()) return end();
        return const_iterator(this, layers_.size(), 0, it);
    }
    size_t count(const std::string& name) const { return lookup(name) ? 1 : 0; }

    // First mount wins: false when the name is already present anywhere.
    bool insert_if_absent(const std::string& name, const E& e, uint32_t bundle = kNoBundle)
    {
        if (!layers_.empty()) {
            const uint64_t h = mount_name_hash(name.data(), name.size());
            for (const Layer& l : layers_) if (probe(l, name, h)) return false;
        }
        Stored s; s.e = e; s.bundle = bundle;
        return overlay_.emplace(name, s).second;
    }

    // Every entry in iteration order, the name as a pointer and length, with
    // nothing allocated: f(const char* name, size_t len, const E& e).
    template <class F>
    void for_each_view(F&& f) const
    {
        for (const Layer& l : layers_)
            for (uint32_t i = 0; i < l.count; ++i)
                f(l.names + l.recs[i].name_off, (size_t)l.recs[i].name_len, l.recs[i].e);
        for (const auto& kv : overlay_) f(kv.first.data(), kv.first.size(), kv.second.e);
    }

    const Overlay& overlay() const { return overlay_; }
    const std::vector<Layer>& layers() const { return layers_; }
    void add_layer(const Layer& l)
    {
        layers_.push_back(l);
        layered_ += l.count;
        names_.emplace_back(new std::atomic<std::string*>[l.count ? l.count : 1]());
    }
    void clear_overlay() { Overlay().swap(overlay_); }
    void clear() { clear_overlay(); drop_names(); layers_.clear(); layered_ = 0; }

    // Serialise the overlay as one layer: records, in the overlay's own
    // iteration order, into `recs`; names into `names`; the hash index into
    // `slots`. Offsets in the records are relative to the start of `names`.
    void write_overlay(std::vector<uint8_t>& recs, std::vector<uint8_t>& slots,
                       std::vector<uint8_t>& names) const
    {
        const size_t n = overlay_.size();
        recs.assign(n * sizeof(Rec), 0);
        uint32_t cap = 16;
        while (cap < n * 2) cap <<= 1;
        std::vector<uint32_t> s(cap, 0);
        size_t i = 0;
        for (const auto& kv : overlay_) {
            Rec r{};
            r.name_off = (uint32_t)names.size();
            r.name_len = (uint32_t)kv.first.size();
            r.bundle = kv.second.bundle;
            r.e = kv.second.e;
            names.insert(names.end(), kv.first.begin(), kv.first.end());
            std::memcpy(recs.data() + i * sizeof(Rec), &r, sizeof(Rec));
            uint32_t at = (uint32_t)mount_name_hash(kv.first.data(), kv.first.size()) & (cap - 1);
            while (s[at]) at = (at + 1) & (cap - 1);
            s[at] = (uint32_t)i + 1;
            ++i;
        }
        slots.resize(cap * sizeof(uint32_t));
        std::memcpy(slots.data(), s.data(), slots.size());
    }

    // Every record and slot in bounds; run once when a layer is mapped.
    static bool valid(const Layer& l)
    {
        if (l.count && !l.recs) return false;
        // At least one empty slot, or a miss would probe forever.
        if (((uint64_t)l.mask + 1) <= (uint64_t)l.count || ((l.mask + 1) & l.mask)) return false;
        for (uint32_t i = 0; i <= l.mask; ++i) if (l.slots[i] > l.count) return false;
        for (uint32_t i = 0; i < l.count; ++i)
            if ((uint64_t)l.recs[i].name_off + l.recs[i].name_len > l.names_size) return false;
        return true;
    }

private:
    // A mapped record's name as a kept std::string. Safe from several threads:
    // the first writer wins and a loser deletes its copy.
    const std::string& name_at(size_t layer, uint32_t index) const
    {
        std::atomic<std::string*>& slot = names_[layer][index];
        if (std::string* s = slot.load(std::memory_order_acquire)) return *s;
        const Layer& l = layers_[layer];
        std::string* made = new std::string(l.names + l.recs[index].name_off, l.recs[index].name_len);
        std::string* expected = nullptr;
        if (!slot.compare_exchange_strong(expected, made, std::memory_order_acq_rel)) {
            delete made;
            return *expected;
        }
        return *made;
    }
    void drop_names()
    {
        for (size_t i = 0; i < names_.size() && i < layers_.size(); ++i)
            for (uint32_t k = 0; k < layers_[i].count; ++k) delete names_[i][k].load();
        names_.clear();
    }

    const Rec* probe(const Layer& l, const std::string& name, uint64_t h) const
    {
        uint32_t at = (uint32_t)h & l.mask;
        for (;;) {
            const uint32_t v = l.slots[at];
            if (!v) return nullptr;
            const Rec& r = l.recs[v - 1];
            if (r.name_len == name.size() && std::memcmp(l.names + r.name_off, name.data(), name.size()) == 0)
                return &r;
            at = (at + 1) & l.mask;
        }
    }

    std::vector<Layer> layers_;
    mutable std::vector<std::unique_ptr<std::atomic<std::string*>[]>> names_;
    size_t layered_ = 0;
    Overlay overlay_;
};

}  // namespace bf6
#endif
