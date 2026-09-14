#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace live_menu {

// One exact public presentation asset already fetched by BF6.  cache_path
// points at the game's numbered HTTP-cache payload; consumers decode it in
// memory and never create an exported intermediate.
struct Asset {
    int id = -1;
    uint64_t expected_size = 0;
    uint64_t last_used = 0;
    std::string url;
    std::string family;
    std::string name;
    std::wstring cache_path;
};

struct Catalog {
    std::wstring manifest_path;
    // Every syntactically valid, allow-listed CDN row in the game's manifest.
    // Some rows no longer have a numbered cache payload but still provide the
    // exact public URL and expected byte size needed for a bounded online GET.
    std::vector<Asset> known_assets;
    // The subset whose numbered payload is still present and size-valid.
    std::vector<Asset> assets;
    int manifest_entries = 0;
    int evicted_entries = 0;
    int rejected_entries = 0;
    std::string error;
};

// Discover the newest Battlefield*6/httpcache/manifest under LOCALAPPDATA.
Catalog discover();

// Explicit path entry point used by controls and diagnostics.
Catalog read(const std::wstring& manifest_path);

// Preserve manifest recency order while selecting every member of the newest
// access cohort for one provider family.  A zero-width cohort includes only
// entries with the exact newest LastUsed value.
std::vector<Asset> newest_cohort(const Catalog& catalog,
                                 const char* family,
                                 uint64_t last_used_window);

// Restrict the newest family cohort to an ASCII case-insensitive token in the
// exact CDN identity (leaf name plus URL).  This remains a cache identity
// filter: it does not infer the service's ordered card records.
std::vector<Asset> newest_cohort_matching(const Catalog& catalog,
                                          const char* family,
                                          uint64_t last_used_window,
                                          const char* identity_token);

// Match one stable CDN identity across the complete validated cache. This is
// for provider relationships whose exact asset name is known: LastUsed is an
// eviction/access clock, so an unselected but still-current category hero can
// legitimately be older than unrelated assets in the same broad family.
std::vector<Asset> matching(const Catalog& catalog,
                            const char* family,
                            const char* identity_token);

// The same exact identity join over every allow-listed manifest row, including
// entries whose numbered payload has been evicted locally.
std::vector<Asset> matching_known(const Catalog& catalog,
                                  const char* family,
                                  const char* identity_token);

// Revalidates the size before returning the exact encoded JPEG/PNG/SVG bytes.
bool read_payload(const Asset& asset, std::vector<uint8_t>& bytes,
                  std::string& error);

// Fetches one manifest-proven public EA CDN object into memory.  Only the
// fixed HTTPS host/prefix accepted by the manifest parser is legal, redirects
// are disabled, and the response must equal the manifest's exact byte size.
bool read_online_payload(const Asset& asset, std::vector<uint8_t>& bytes,
                         std::string& error);

} // namespace live_menu
