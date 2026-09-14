# Opening a level quickly

Opening a level (`bf6_open` then `bf6_open_level`) used to repeat the same work in every
process: mount the archives, decrypt and read the type schema, index every partition,
and walk the level. Three of those four steps give the same answer every time for the
same installation, so they are now done once and stored or made much cheaper. The walk
still runs.

Measured on the EA install, warm file cache, `test/level_open_bench.cpp`:

| Step | Before | Now |
| --- | --- | --- |
| `bf6_open` (shared archives) | 0.44 s | 0.025 s |
| Mount the level's archives | 1.5 to 7.3 s | 0.04 to 0.05 s |
| Type schema (EA build, decrypted in memory) | 4.3 s | 0.05 s |
| Partition index | 0.5 to 0.9 s | about 0.17 s |
| Level walk | 0.8 s | 0.8 s |
| **`bf6_open_level`, mp_dumbo** | **about 4.2 s warm, 11.5 s cold** | **about 1.05 s** |

Every placement row is identical with and without the stored data
(`level_open_bench` digests), and so is every mount table entry
(`mount_snapshot_test`).

What that does for the tools:

- The whole-installation up-front cache (28 maps, EA install) builds in 299 s against
  about 8 minutes, and every map's placement file is byte-identical to the earlier
  build. The build now opens each map in a fresh context, as an editor does, so each
  map's stored mount is the one a later editor session reuses.
- The Unreal MP_Isolated load bench: scene preparation 42 s to 32 s, the full load
  40 s to 31 s, with the same 51,951 placements, 3,801 meshes, 4,849 bindings and
  2,823 textures.

## The mount snapshot

`Source::mount_tocs` mounts a list of TOCs in order. The tables a mount produces depend
only on which TOCs were mounted, in what order, and what was mounted before, so that
sequence is the key: each TOC's normalised path, size and modification time, folded in
order onto the key of the mount before it. A failed TOC is part of the sequence too.

- **Hit:** the file is memory-mapped and its tables are read in place. Nothing is read
  from the archives, and nothing is built per entry.
- **Miss:** the TOCs are mounted as before, what this call added is written as a new
  layer, and the tables are reopened from that file, so the process also stops holding
  them as heap strings.

A table (`src/mount_table.h`) is a stack of read-only mapped layers plus one ordinary
map for anything mounted later. Layers are searched first, which keeps the rule that the
first mount of a name wins. Each layer is fixed-size records over a name blob, with an
open-addressed hash index. Iterating a table yields `first` and `second` exactly as the
old maps did, and both stay valid for the table's lifetime: a mapped name becomes a
`std::string` the first time something reads it.

An ordinary level open stores three layers:

1. `bf6_open`'s archives (`Data/Win32/*.toc`),
2. the rest of the shared archives,
3. the level's own archives.

The first two are shared by every level. Each level adds about 24 MB (mp_isolated after
mp_dumbo: 91 MB, then 115 MB). A second level opened in the same context mounts in
memory rather than stacking a fourth layer, so every stored file is one that an ordinary
open asks for.

Files live in `%LOCALAPPDATA%/BF6HighPoly/Cache/mount`, named
`<install root>-<first layer key>-<layer key>.bf6m`.

- A game patch changes every key. When a new first layer is saved, each other chain of
  the same installation folder is checked against the TOC stamps its first layer
  recorded; a chain whose TOCs changed or are gone is deleted. A chain that started
  from a different archive set (a front-end context, a tool) is still valid and stays.
- Each installation folder is capped at 2 GB, least recently used first. A hit marks the
  file as used.
- The header records the format version and the record sizes, so a build whose layout
  differs rejects the file.
- Every section, slot and name reference is bounds-checked when a file is mapped.

`BF6_DISABLE_MOUNT_SNAPSHOT=1` mounts everything directly, which is the control used by
the parity tests.

## The partition index beside it

The partition index (partition GUID to name) is a function of the mounted tables and the
read mode. When every table is a snapshot layer, it is stored next to the layers
(`...-<key>-pidx.bin`) and read back instead of reading one header from every partition.
It is not used when anything has been mounted outside a snapshot.

## The type schema

An EA App build encrypts the executable's reflection sections. They are decrypted in
this process's memory from the user's own licence (`src/ooa_lift.inc`), and nothing is
written to disk. That decrypt was one-byte-at-a-time software AES on one thread. CBC
decryption is independent per block, so it now splits the sections across threads and
uses the processor's AES instructions where present. The image is byte-identical to the
original path (`test/types_open_bench.cpp`, which also runs the original path under
`BF6_OOA_LIFT_SOFT=1`).

## The walk

Opening a level by a bare name (`mp_dumbo`) used to build an alias map of every
partition's name and leaf before it could resolve the level root. The first few misses
now scan the mapped names without allocating, and give the same first match. The map is
only built if a walk keeps missing.

## Tests

- `mount_snapshot_test <game> <level>` compares a direct mount with a snapshot miss and
  a snapshot hit: every res, ebx and chunk entry, bundles, depots, per-type counts,
  sampled payload reads, mounting another level afterwards, and a second level reusing
  the shared layers.
- `level_open_bench <game> <level>...` times the public API and digests every placement
  row. Run it with and without `BF6_DISABLE_MOUNT_SNAPSHOT=1`.
- `types_open_bench <exe>` times the schema open and checks the fast decrypt against the
  original.
