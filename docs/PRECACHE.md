# Up-front cache (precache)

One pass, run by the core, that reads everything the High Poly tools need for every
map of a player's installation and writes it to one engine-neutral cache. Godot and
Unreal both read that same cache, so after it finishes any map opens without touching
the game files, in either engine.

Status: phase 1 implemented (cache store, key, manifests, pack files, shared store,
thread pool, progress model and the `bf6_precache_*` C API, with synthetic self-test
layers and `test/precache_test.cpp`). No game layers yet.

## Product behaviour (both engines)

1. When the Battlefield 6 installation is selected (or first detected), the tool opens a
   screen that covers the editor and starts the precache immediately.
2. The screen shows: overall progress, one row per map with per-layer progress
   (placements, terrain, ground, roads/decals, water, lights, FX, scatter, props,
   gamemode), the shared asset store, and the object preview cache.
3. Every High Poly control stays disabled until the cache for the current installation
   is complete. Only the plugin update check remains available.
4. Entering high poly on any map afterwards loads only from the cache.
5. Removed from both UIs: "Prepare maps", the game-files "Check for updates" (the core
   detects game updates itself), and "Build object previews". One "Check for plugin
   updates" button remains, shown when the plugin is not managed by the BF6 patch.

## Cache identity and invalidation

```
<cache root>/bf6hp-cache/v<FORMAT>/<key16>/
    root.json            format, key inputs, created, last used, core version
    shared/              content-addressed store (meshes, textures, sheets, objects)
    maps/<level>/        per-map packs + complete.json (written last, atomically)
    previews/<engine>/   engine-rendered thumbnails (engines differ in look)
```

`key16 = sha256(install_identity | core_cache_recipe | FORMAT)[:16]`

- `install_identity`: `bf6_cache_identity.h` (game build and archive fingerprint).
- `core_cache_recipe`: a constant compiled into the core, bumped by hand whenever any
  reader change alters cached output. Presentation or engine-side code changes do not
  touch it, so ordinary plugin updates keep the cache.
- Per-layer versions live in `complete.json`, so one layer can be rebuilt without
  redoing the others.
- Engine-rendered previews add the engine name and a preview epoch to their own key.
- On start the core removes sibling roots with other keys, after the new root is valid.
- `<cache root>` is shared per user (`%LOCALAPPDATA%/BF6HighPoly`), so a player with both
  engines precaches once.

## Speed plan

Measured on the current Godot path (7950X3D, NVMe), all 28 maps: about 1.25-2 h, and
even warm loads repeat most of the work. The precache must be much faster than that.
Levers, in order of expected gain:

1. **Open the installation once, not once per map.** Today each map (and each worker
   process) mounts the archives (~10 s), builds the index (~7 s), reads the type
   database from the executable (177 MB, every open) and runs the OOA lift. The
   precache holds one context for the whole run.
2. **Content-addressed shared store.** Meshes, textures, material records and FX sheets
   are shared heavily across maps. Each unique resource is decoded and written once,
   keyed by its resource id and chunk hash; maps store references.
3. **No texture re-encoding.** Game textures are already GPU block-compressed. Store the
   BCn payload and mip chain as shipped (plus format/size); engines upload directly.
   Today Godot decodes to images and back.
4. **Parallel decode.** A thread pool sized to physical cores runs Oodle decompression,
   mesh decode, texture extraction and terrain composition. Placement walks and
   per-map layers run concurrently, limited by a memory budget rather than a fixed
   count.
5. **Pack files, not tens of thousands of small files.** Today's geometry cache is
   ~67,000 files; file creation dominates on Windows. Each map writes a few packs and
   the shared store appends to segment files with an index. Loads memory-map packs.
6. **Terrain and roads computed once per map** in the core (heights, ground composite,
   far terrain, road drape), stored as compressed buffers. These are redone on every
   Godot session today.
7. **Stream write, verify by hash, no second read.**

Targets to prove with measurements before promising: whole-install precache well
under the current 28 min geometry-only pass; any cached map ready to display in a few
seconds. Every phase below ships with before/after timings.

## Layers

| Layer | Stored as | Produced by |
|---|---|---|
| placements / groups | per-map pack: group keys, float32 transforms, layer tags, variation | walk + map grouping (moved into core) |
| terrain heights | compressed r16 + metadata + tile steps | terrain composite |
| terrain mesh inputs | chunk tables (engines build meshes from heights quickly) | core |
| ground materials | ground / sheet / far-ground packets, compressed | environment ground |
| roads, decal volumes | vertex/index buffers + material records + sheet refs | roads drape, decals |
| water | surfaces, height/mask atlases, textures, water simulation parameters | water reader (simulation parameters exported for cache-only animation) |
| lights, lighting, sky | records + sky panorama | level lights, environment lighting, lighting zones |
| FX | effect records + sheet refs | fx |
| scatter | rows | scatter |
| gamemode | markers | gamemode |
| props (original map assets) | refs into shared meshes / materials / textures | mesh + material + texture readers |
| object catalogue | object descriptors (meshes, transforms, material refs) | catalogue |
| previews | engine PNGs | engine, from object descriptors, after the core run |

Not cacheable: close-up ground detail that rebuilds as the camera moves. In cache-only
mode it is disabled or served from a fixed precomputed page set.

## C API (implemented in bf6_core.h, additive to interface version 6)

The exported names are `bf6_precache_*` (the C++ namespace `bf6_cache` is taken by
`bf6_cache_identity.h`). The sketch below shows the shape; the header is authoritative.

```c
typedef struct bf6_cache bf6_cache;

/* Opens or creates the cache for this installation. Never touches game files
 * beyond the identity check. */
bf6_cache* bf6_cache_open(const char* game_dir, const char* cache_root, char* err, int err_len);
void       bf6_cache_close(bf6_cache*);

/* 1 when every map and the shared store are complete for the current key. */
int bf6_cache_ready(bf6_cache*);
int bf6_cache_map_ready(bf6_cache*, const char* level);

/* Starts the whole-install build on core-owned threads. Non-blocking. */
int  bf6_cache_build_start(bf6_cache*, const char* const* levels, int level_count, int flags);
void bf6_cache_build_cancel(bf6_cache*);   /* stops after the current unit; resumable */

/* Snapshot of progress; safe to call every frame from the UI thread. */
typedef struct {
    int32_t struct_size;
    int32_t state;            /* idle, running, cancelling, done, failed */
    double  overall;          /* 0..1, monotonic, weighted by measured cost */
    int32_t map_count, maps_done;
    int64_t bytes_written;
    char    current_map[64];
    char    current_layer[32];
    char    current_item[128]; /* "roads: 812 / 1435 records" */
    double  seconds_since_update;
} bf6_cache_progress;
int bf6_cache_progress_get(bf6_cache*, bf6_cache_progress* out);

typedef struct {
    char    level[64];
    int32_t state;            /* waiting, running, done, failed */
    double  progress;         /* 0..1 */
    double  layer_progress[16];
} bf6_cache_map_progress;
int bf6_cache_map_progress_get(bf6_cache*, bf6_cache_map_progress* out, int out_max);

/* Cache-only readers: same result structs as the live readers. */
```

The engine adapters own only: the cover screen, uploading cached buffers into engine
meshes/textures/materials, rendering previews from object descriptors, and gating
their controls on `bf6_cache_ready`.

## Phases

1. (done) Cache root, key, manifests, progress API, thread pool, pack writer/reader (no layers).
   Unit tests with synthetic data (`-DBF6_BUILD_CORE_TESTS=ON`). Timing harness: next.
2. Single-context whole-install scan (mount, index, types, lift once) and the
   content-addressed store for meshes and textures. Measure against today.
3. Terrain, ground, roads, water (including exported simulation parameters).
4. Placements/grouping moved into the core; lights, FX, scatter, gamemode, catalogue.
5. Godot adapter: cover screen, gating, cache-only loading, previews; remove old
   preparation and buttons.
6. Unreal adapter: same.
7. Release: both engines on the same core tag.
