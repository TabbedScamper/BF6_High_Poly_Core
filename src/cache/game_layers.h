// Game layers for the up-front cache: what one map needs, read through the public
// core API and written to packs and the shared store. See docs/PRECACHE.md.
#pragma once
#include "bf6_core.h"
#include "pack.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace bf6::cache {

// Record formats written by these layers. Bump the matching version when a
// record's bytes change; complete.json records it per layer.
inline constexpr std::uint32_t kPlacementsVersion = 1;
// 2: mesh references (materials and decisions; geometry read from the installation).
inline constexpr std::uint32_t kMeshesVersion = 2;
// 2: texture references into the installation instead of decoded pixels.
inline constexpr std::uint32_t kTexturesVersion = 2;
// 2: terrain and water heightfield references.
inline constexpr std::uint32_t kTerrainVersion = 2;

struct GameBuildOptions {
    int texture_max_dim = 0;      // applied when loading; references are size-independent
    std::size_t threads = 0;      // 0 = hardware threads
};

using Report = std::function<void(const char* layer, double fraction, const std::string& item)>;

struct GameLayerResult {
    std::uint64_t placement_bytes = 0, terrain_bytes = 0;
    int placements = 0, mesh_reads = 0, meshes_new = 0, meshes_failed = 0;
    int textures_new = 0, textures_failed = 0;
    std::uint64_t mesh_bytes = 0, texture_bytes = 0;
    double seconds_open = 0, seconds_meshes = 0, seconds_textures = 0, seconds_terrain = 0, seconds_write = 0;
    // What mesh bytes are made of, to size a reference format.
    std::uint64_t sections = 0, sections_with_colours = 0, sections_uv_swapped = 0;
    std::uint64_t colour_bytes = 0, geometry_bytes = 0, material_bytes = 0;
    std::string placements_digest, terrain_digest;
};

// Builds every game layer of one level on an open context. The context must not
// be used by anyone else during the call. Writes <map dir>/placements.* and
// terrain.* packs; meshes and textures go to the shared store once per install.
bool build_game_level(bf6_ctx* ctx, const std::string& level, const fs::path& map_dir,
                      ContentStore& shared, const GameBuildOptions& options,
                      const Report& report, const std::atomic<bool>& cancel,
                      GameLayerResult& result, std::string& err);

// Shared-store keys, also used by loaders.
std::string mesh_key(const std::string& res, const std::string& bundle, const std::string& variation);
std::string texture_key(const std::string& res);

}  // namespace bf6::cache
