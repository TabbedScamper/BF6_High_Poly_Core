# Reader performance log

Measured with `test/precache_bench.cpp` on the author's machine (32 hardware
threads, NVMe, EA App install of Battlefield 6). Game files were recently read,
so these are warm-file-cache numbers. Every change here must keep the content
digest (a hash over every mesh buffer and texture payload the bench reads)
identical to the build before it.

## 2026-09-13: type schema reuse, texture copy removal, block output sizing

| | Before | After |
|---|---|---|
| `bf6_open_level`, MP_Dumbo | 6.1 s | 2.1 s |
| `bf6_open_level`, MP_Isolated | 6.8-7.7 s | 2.5-2.6 s |
| 600 mesh reads incl. textures, one thread, MP_Dumbo | 2.50 s | 1.21 s |
| 600 mesh reads incl. textures, one thread, MP_Isolated | 4.46 s | 2.39 s |
| Texture decode, one thread | 770-830 MB/s | 1,290-1,370 MB/s |
| Texture decode, 16 threads on one context | (not possible) | 4.6-6.1 GB/s |

Changes:

1. `bf6_open_level` keeps a readable type schema from an earlier open instead of
   re-reading it from the game executable for every level (it does not depend on
   the level).
2. `Texture::dims_for` moves the decompressed payload into the image instead of
   copying it.
3. `cas_read` sizes a multi-block output once from the block headers instead of
   growing it block by block (texture chunks are many blocks; growth reallocated
   and copied the payload repeatedly). Every archive read benefits.
4. New `bf6_texture_decode_res`: a caller-owned texture decode that never touches
   the context's texture table, so many threads can decode on one mount.

Equivalence: content digests identical before and after on MP_Dumbo (1,476 meshes,
582 textures, 1.8 GB: `4e6975ec42b08ece`) and MP_Isolated (1,428 meshes, 1,121
textures, 4.5 GB: `ff7729e92e92eae1`).

Other measurements that shape the precache:

- First archive mount 6 s; later maps on the same context 0.5-1.1 s.
- Extra contexts cost 9-12 s each and scaled poorly (1.2-1.8x on 3 threads), so the
  precache parallelises inside one context instead.
- Unique (mesh, bundle, variation) reads are 9-27% of placements per map; across
  4 maps a shared store reads 28% fewer meshes than per-map reads.
- Terrain heightfield read: 1.0-2.3 s per map.
