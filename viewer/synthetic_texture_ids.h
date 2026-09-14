#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace bf6_viewer {

// Content-identity registry for CPU-generated textures.
//
// Native libbf6 texture ids are non-negative and stable for the context. UI
// SVG rasters and service-cache images need the same invariant even though
// they have no native id. A route-local decrementing counter does not provide
// it: a later route can reuse an earlier route's id while the GPU cache still
// owns the earlier SRV. This registry derives a negative candidate from the
// content identity and retains both directions to resolve the unlikely hash
// collision without ever aliasing two keys.
class SyntheticTextureIds {
public:
    int id_for(const std::string& domain, const std::string& identity)
    {
        if (domain.empty() || identity.empty()) return -1;
        const std::string key = domain + '\0' + identity;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto known = ids_.find(key);
        if (known != ids_.end()) return known->second;

        uint64_t hash = 1469598103934665603ull;
        for (unsigned char byte : key)
            hash = (hash ^ byte) * 1099511628211ull;

        static constexpr int kFirst = -100000000;
        static constexpr int kLast = -1899999999;
        static constexpr uint64_t kSpan =
            static_cast<uint64_t>(-static_cast<int64_t>(kLast) + kFirst + 1);
        int candidate = kFirst - static_cast<int>(hash % kSpan);
        for (;;)
        {
            const auto owner = owners_.find(candidate);
            if (owner == owners_.end()) break;
            if (owner->second == key) return candidate;
            candidate = candidate == kLast ? kFirst : candidate - 1;
        }
        ids_.emplace(key, candidate);
        owners_.emplace(candidate, key);
        return candidate;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ids_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, int> ids_;
    std::map<int, std::string> owners_;
};

} // namespace bf6_viewer
