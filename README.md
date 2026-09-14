# BF6 High Poly Core

The shared native core for the BF6 High Poly tools. The Godot plugin
(`BF6_High_Poly_Godot_Plugin`) and the Unreal tools (`BF6_Unreal_SDK`,
`BF6_Unreal_SDK_High_Poly`) build from these same files, so a core upgrade reaches
both engines.

It reads a player's own Battlefield 6 installation (archives, Oodle from the
installed game, type data), decodes meshes, materials, textures, terrain, water,
placements, lights, FX and UI data, and hands engine-neutral results across the
plain C API in `include/bf6_core.h`. Engine bindings only upload that data, convert
coordinates and build scene objects. No game data is included in this repository.

## Licence and credit

Apache License 2.0 (`LICENSE`). Redistributions and anything built on this core must keep
`NOTICE` and credit **TabbedScamper - BF6 High Poly Core**.

## Interface version

`BF6_ABI_VERSION` in `include/bf6_core.h` is the contract between this library and
the engine bindings. Bindings check `bf6_abi_version()` before calling anything that
takes a struct. Additive exports keep the number; any change to an existing
struct layout or signature increments it.

Version 6 unifies the former Godot (4) and Unreal (5) copies of this code.

## Building

Windows, CMake 3.20+, Visual Studio 2022 or newer:

```
cmake -S . -B out-build -A x64
cmake --build out-build --config Release --target bf6_core
```

This produces `bf6_core.dll` (shared, for engine bindings) and optionally
`bf6_core_static`. `cmake --install` lays out `bin/`, `lib/` and `include/`.

Tests, probes and research tools are off by default. Many need a local game
install (`BF6_TEST_GAME_DIR`) and some reference research sources that are not part
of this repository:

```
cmake -S . -B out-build -A x64 -DBF6_BUILD_TOOLS=ON
```

## Layout

- `include/bf6_core.h` - the public C API.
- `src/` - readers and the API implementation (`*_ext.inc` units are compiled into
  `bf6_core.cpp`).
- `ui_runtime/` - installed UI sound playback used by the core, plus UI runtime pieces
  used by tools.
- `viewer/` - the standalone native viewer and its UI adapters (tools build only).
- `third_party/imgui/` - Dear ImGui with its Win32 and Direct3D 11 backends, for the
  viewer only (MIT, `third_party/imgui/LICENSE.txt`).
- `test/`, `probes/`, `tools/` - validation and packaging tooling.
- `docs/` - `PRECACHE.md` (the up-front cache) and `LEVEL-OPEN.md` (the mount
  snapshot and the other stored or accelerated steps of opening a level).

## Using the core from an engine

Each engine repository carries this repository as a git submodule, pinned to a
commit, rather than a copy of its files:

- Godot plugin: `core/`. `native/build.bat` builds `bf6_core` from it and links the
  binding against the result.
- Unreal SDK: `Source/ThirdParty/libbf6/core/`. `Tools/build-core.ps1` builds a
  package from it and stages `include/` and `bin/Win64/` beside it.

To upgrade both engines, commit here, then move each submodule to that commit and
rebuild. `tools/build_core_package.py` records the source commit in
`reader-manifest.json`, so a staged binary names the core it came from.
