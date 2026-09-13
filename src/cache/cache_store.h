// Up-front cache: root, key, manifests and stale-root cleanup.
// See docs/PRECACHE.md. Engine-neutral, C++17, no game reads.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bf6::cache {

namespace fs = std::filesystem;

// Layout of everything under <cache root>/bf6hp-cache/v<kFormat>/. Bump when the
// directory structure or any file format below changes.
inline constexpr std::uint32_t kFormat = 1;

// Bump by hand whenever a reader change alters cached output. Presentation and
// engine-side changes never touch it, so ordinary plugin updates keep the cache.
inline constexpr std::uint32_t kRecipe = 1;

// 16 hex characters derived from (install identity, recipe, format).
std::string cache_key(const std::string& install_identity,
                      std::uint32_t recipe = kRecipe, std::uint32_t format = kFormat);

// Writes a file so a reader sees either the old bytes or the complete new bytes:
// temporary sibling, flush, then rename over the target. Returns false with err.
bool atomic_write(const fs::path& target, const std::string& bytes, std::string& err);

// Reads a whole file. Returns false if missing or unreadable.
bool read_file(const fs::path& path, std::string& out);

// True when any existing component from `base` down to `path` is a symlink or
// junction. The cache never follows links.
bool has_link_below(const fs::path& base, const fs::path& path);

struct LayerRecord {
    std::uint32_t version = 0;
    std::uint64_t bytes = 0;
    std::string digest;   // hex fingerprint of the layer's files
};

class Store {
public:
    // Opens (creating if needed) the cache for this installation identity under
    // cache_root. Validates or writes root.json. Does not delete anything.
    bool open(const fs::path& cache_root, const std::string& install_identity, std::string& err);

    const fs::path& root() const { return root_; }
    const std::string& key() const { return key_; }
    fs::path shared_dir() const { return root_ / "shared"; }
    fs::path map_dir(const std::string& level) const;

    // A map is ready only when complete.json exists, names this key and every
    // required layer is present at at least the required version.
    bool map_complete(const std::string& level,
                      const std::map<std::string, std::uint32_t>& required) const;

    // Written last, atomically, after every layer file of the map is durable.
    bool write_map_complete(const std::string& level,
                            const std::map<std::string, LayerRecord>& layers, std::string& err);

    // Marks the whole install complete (every requested map plus shared data).
    bool write_install_complete(const std::vector<std::string>& levels, std::string& err);
    bool install_complete(const std::vector<std::string>& levels) const;

    // Removes caches for other keys and older formats. Only directories this
    // code created (they carry a matching root.json marker) are removed, never
    // through a link. Call after open() succeeded. Returns the number removed.
    int sweep_stale(std::string& err) const;

    static bool valid_level_name(const std::string& level);

private:
    fs::path base_;       // <cache root>/bf6hp-cache
    fs::path root_;       // <base>/v<format>/<key>
    std::string key_;
    std::string identity_;
};

}  // namespace bf6::cache
