cmake_minimum_required(VERSION 3.20)
project(libbf6 CXX)

# The engine-neutral Battlefield 6 decode core. No Godot, no Unreal, no engine
# headers here - only the game formats. Bindings link against this.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BF6_SOURCES
    src/bf6_core.cpp
    src/oodle.cpp
    src/cas.cpp
    src/dbobject.cpp
    src/caslocator.cpp
    src/toc.cpp
    src/bundle.cpp
    src/source.cpp
    src/armory.cpp
    src/meshset.cpp
    src/meshset_sections.cpp
    src/placeables.cpp
    src/types.cpp
    src/ebx.cpp
    src/walk.cpp
    src/terrain.cpp
    src/texture.cpp
    src/depot.cpp
    src/destruction.cpp
    src/decals.cpp
    src/water_tree.cpp
    src/terrain_mesh.cpp
    src/decal_draws.cpp
    src/map_validate.cpp
    src/mode_setup.cpp
    src/scatter_layout.cpp
    src/game_log.cpp
    src/caps.cpp
    src/budget.cpp
    src/blocks.cpp
    src/spatial_export.cpp
    src/walk_mode.cpp
    src/loadout.cpp
    src/splat.cpp
    src/terrainlayers.cpp
    src/terrainstatic.cpp
    src/terrainjoin.cpp
    src/terrainshader.cpp
    src/terrainmask.cpp
    src/terraintextures.cpp
    src/terrainpage.cpp
    src/terraincomposite.cpp
    src/groundsplat.cpp
    src/velighting.cpp
    src/levellights.cpp
    src/lightingzones.cpp
    src/fx.cpp
    src/expression_graph.cpp
    src/expression_registry.cpp
    src/expression_vm.cpp
    src/cache/cache_store.cpp
    src/cache/pack.cpp
    src/cache/progress.cpp
    src/cache/cache_api.cpp
    src/cache/game_layers.cpp
)

# bf6_core.cpp deliberately keeps the ABI implementation in small include
# units.  Register those units as header-only sources so changing one always
# invalidates bf6_core.cpp instead of silently relinking a stale object.
file(GLOB BF6_INCLUDED_SOURCES CONFIGURE_DEPENDS src/*_ext.inc)
set_source_files_properties(${BF6_INCLUDED_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(bf6_core SHARED ${BF6_SOURCES} ${BF6_INCLUDED_SOURCES})
target_include_directories(bf6_core PUBLIC include PRIVATE src)
# Only the shared build tags the public functions for DLL export. The static
# library (linked into Unreal) leaves BF6_API empty.
target_compile_definitions(bf6_core PRIVATE BF6_BUILDING_DLL)

# A static form too, for native tools that prefer one executable. Engine
# bindings currently consume the shared C ABI so both Godot and Unreal can
# verify bf6_abi_version() before touching any struct-bearing export.
add_library(bf6_core_static STATIC ${BF6_SOURCES} ${BF6_INCLUDED_SOURCES})
target_include_directories(bf6_core_static PUBLIC include PRIVATE src)

# One canonical package layout for every binding. This prevents a copied DLL
# and copied header from drifting independently inside an engine plugin.
install(TARGETS bf6_core
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION lib)
install(TARGETS bf6_core_static ARCHIVE DESTINATION lib)
install(FILES include/bf6_core.h DESTINATION include)

# Installed UI sound playback (bf6_ui_sound_* in uisound_ext.inc) is part of the
# shared core on Windows.
if(WIN32)
    foreach(core_target bf6_core bf6_core_static)
        target_sources(${core_target} PRIVATE ui_runtime/installed_sound_wave.cpp)
        target_include_directories(${core_target} PRIVATE ui_runtime)
    endforeach()
    target_link_libraries(bf6_core PRIVATE xaudio2 ole32)
endif()

if (MSVC)
    target_compile_options(bf6_core PRIVATE /W4)
    target_compile_options(bf6_core_static PRIVATE /W4)
endif()

# Everything below is validation and research tooling. Several targets need a
# local game install, and some reference research-only sources that are not
# part of this repository. Engine packages need only the libraries above.
# Core unit tests that need no game install and no research sources.
option(BF6_BUILD_CORE_TESTS "Build self-contained core tests" OFF)
if(BF6_BUILD_CORE_TESTS)
    enable_testing()
    add_executable(precache_test test/precache_test.cpp)
    target_link_libraries(precache_test PRIVATE bf6_core_static)
    target_include_directories(precache_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(precache_test PRIVATE xaudio2 ole32)
    endif()
    add_test(NAME core.precache COMMAND precache_test "${CMAKE_CURRENT_BINARY_DIR}/precache_test_root")
    # Real precache build runner (not a CTest test).
    add_executable(precache_run test/precache_run.cpp)
    target_link_libraries(precache_run PRIVATE bf6_core_static)
    target_include_directories(precache_run PRIVATE src include)
    if(WIN32)
        target_link_libraries(precache_run PRIVATE xaudio2 ole32)
    endif()
    add_executable(precache_ue_parity test/precache_ue_parity.cpp)
    target_link_libraries(precache_ue_parity PRIVATE bf6_core_static)
    target_include_directories(precache_ue_parity PRIVATE src include)
    if(WIN32)
        target_link_libraries(precache_ue_parity PRIVATE xaudio2 ole32)
    endif()
    add_executable(precache_texbench test/precache_texbench.cpp)
    target_link_libraries(precache_texbench PRIVATE bf6_core_static)
    target_include_directories(precache_texbench PRIVATE src include)
    if(WIN32)
        target_link_libraries(precache_texbench PRIVATE xaudio2 ole32)
    endif()
    # Timing harness against a real installation (not a CTest test).
    add_executable(precache_bench test/precache_bench.cpp)
    target_link_libraries(precache_bench PRIVATE bf6_core_static)
    target_include_directories(precache_bench PRIVATE src include)
    if(WIN32)
        target_link_libraries(precache_bench PRIVATE xaudio2 ole32)
    endif()
    # Public-API level open time with a digest of every placement row.
    add_executable(level_open_bench test/level_open_bench.cpp)
    target_link_libraries(level_open_bench PRIVATE bf6_core_static)
    target_include_directories(level_open_bench PRIVATE src include)
    if(WIN32)
        target_link_libraries(level_open_bench PRIVATE xaudio2 ole32)
    endif()
    # Type schema open time; fast decrypt checked against the original.
    add_executable(types_open_bench test/types_open_bench.cpp)
    target_link_libraries(types_open_bench PRIVATE bf6_core_static)
    target_include_directories(types_open_bench PRIVATE src include)
    if(WIN32)
        target_link_libraries(types_open_bench PRIVATE xaudio2 ole32)
    endif()
    # Mount snapshot parity and timing against a real installation.
    add_executable(loadout_test test/loadout_test.cpp)
    target_link_libraries(loadout_test PRIVATE bf6_core_static)
    target_include_directories(loadout_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(loadout_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(walk_mode_test test/walk_mode_test.cpp)
    target_link_libraries(walk_mode_test PRIVATE bf6_core_static)
    target_include_directories(walk_mode_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(walk_mode_test PRIVATE xaudio2 ole32)
    endif()
    # The walker's numbers come from the game, and using them changes the walk.
    add_executable(walk_tuning_test test/walk_tuning_test.cpp)
    target_link_libraries(walk_tuning_test PRIVATE bf6_core_static)
    target_include_directories(walk_tuning_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(walk_tuning_test PRIVATE xaudio2 ole32)
    endif()
    # The first-person soldier: arms and weapon, anchored on the eye.
    add_executable(soldier_1p_view_test test/soldier_1p_view_test.cpp)
    target_link_libraries(soldier_1p_view_test PRIVATE bf6_core_static)
    target_include_directories(soldier_1p_view_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(soldier_1p_view_test PRIVATE xaudio2 ole32)
    endif()
    # The DiceExpression corpus check, under the core-tests guard so it can be
    # run without BF6_BUILD_TOOLS (which collides with other targets). Same
    # source as the tools-side expression_test; a tiling change must be measured
    # against the whole shipped corpus before it is trusted.
    add_executable(expr_corpus_test test/expression_test.cpp)
    target_link_libraries(expr_corpus_test PRIVATE bf6_core_static)
    target_include_directories(expr_corpus_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(expr_corpus_test PRIVATE xaudio2 ole32)
    endif()
    # A DiceExpression graph, disassembled with its operators named.
    add_executable(expr_disasm test/expr_disasm.cpp)
    target_link_libraries(expr_disasm PRIVATE bf6_core_static)
    target_include_directories(expr_disasm PRIVATE src include)
    if(WIN32)
        target_link_libraries(expr_disasm PRIVATE xaudio2 ole32)
    endif()
    # Why the executable cannot name most operator keys: candidate shapes and
    # hash functions, each measured against a random-key control.
    add_executable(expr_name_probe test/expr_name_probe.cpp)
    target_link_libraries(expr_name_probe PRIVATE bf6_core_static)
    target_include_directories(expr_name_probe PRIVATE src include)
    if(WIN32)
        target_link_libraries(expr_name_probe PRIVATE xaudio2 ole32)
    endif()
    # Can the reflected registry name its own operators by pointer, checked
    # against the names the hash route already proved?
    add_executable(expr_registry_probe test/expr_registry_probe.cpp)
    target_link_libraries(expr_registry_probe PRIVATE bf6_core_static)
    target_include_directories(expr_registry_probe PRIVATE src include)
    if(WIN32)
        target_link_libraries(expr_registry_probe PRIVATE xaudio2 ole32)
    endif()
    # How fast the game's own locomotion cycles actually travel.
    add_executable(loco_speed_probe test/loco_speed_probe.cpp)
    target_link_libraries(loco_speed_probe PRIVATE bf6_core_static)
    target_include_directories(loco_speed_probe PRIVATE src include)
    if(WIN32)
        target_link_libraries(loco_speed_probe PRIVATE xaudio2 ole32)
    endif()
    # Which joint is actually the soldier's eye.
    add_executable(eye_joint_probe test/eye_joint_probe.cpp)
    target_link_libraries(eye_joint_probe PRIVATE bf6_core_static)
    target_include_directories(eye_joint_probe PRIVATE src include)
    if(WIN32)
        target_link_libraries(eye_joint_probe PRIVATE xaudio2 ole32)
    endif()
    # The first-person chain: skeleton, arms mesh, rig, clips.
    add_executable(soldier_1p_probe test/soldier_1p_probe.cpp)
    target_link_libraries(soldier_1p_probe PRIVATE bf6_core_static)
    target_include_directories(soldier_1p_probe PRIVATE src include)
    if(WIN32)
        target_link_libraries(soldier_1p_probe PRIVATE xaudio2 ole32)
    endif()
    # Where the soldier's authored movement values live.
    add_executable(soldier_movement_probe test/soldier_movement_probe.cpp)
    target_link_libraries(soldier_movement_probe PRIVATE bf6_core_static)
    target_include_directories(soldier_movement_probe PRIVATE src include)
    if(WIN32)
        target_link_libraries(soldier_movement_probe PRIVATE xaudio2 ole32)
    endif()
    # Every role stands in its own pose, and all four come from the _01 take.
    add_executable(role_pose_test test/role_pose_test.cpp)
    target_link_libraries(role_pose_test PRIVATE bf6_core_static)
    target_include_directories(role_pose_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(role_pose_test PRIVATE xaudio2 ole32)
    endif()
    # Which front-end idle each role resolves to, front-end mount vs full.
    add_executable(idle_clip_probe test/idle_clip_probe.cpp)
    target_link_libraries(idle_clip_probe PRIVATE bf6_core_static)
    target_include_directories(idle_clip_probe PRIVATE src include)
    if(WIN32)
        target_link_libraries(idle_clip_probe PRIVATE xaudio2 ole32)
    endif()
    # The skinned soldier record: bind identity, and the posed skin reproducing
    # the static soldier it replaces.
    add_executable(soldier_skin_test test/soldier_skin_test.cpp)
    target_link_libraries(soldier_skin_test PRIVATE bf6_core_static)
    target_include_directories(soldier_skin_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(soldier_skin_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(level_paths_test test/level_paths_test.cpp)
    target_link_libraries(level_paths_test PRIVATE bf6_core_static)
    target_include_directories(level_paths_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(level_paths_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(spatial_export_test test/spatial_export_test.cpp)
    target_link_libraries(spatial_export_test PRIVATE bf6_core_static)
    target_include_directories(spatial_export_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(spatial_export_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(budget_blocks_test test/budget_blocks_test.cpp)
    target_link_libraries(budget_blocks_test PRIVATE bf6_core_static)
    target_include_directories(budget_blocks_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(budget_blocks_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(log_caps_test test/log_caps_test.cpp)
    target_link_libraries(log_caps_test PRIVATE bf6_core_static)
    target_include_directories(log_caps_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(log_caps_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(mode_scatter_test test/mode_scatter_test.cpp)
    target_link_libraries(mode_scatter_test PRIVATE bf6_core_static)
    target_include_directories(mode_scatter_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(mode_scatter_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(map_validate_test test/map_validate_test.cpp)
    target_link_libraries(map_validate_test PRIVATE bf6_core_static)
    target_include_directories(map_validate_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(map_validate_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(decal_draws_test test/decal_draws_test.cpp)
    target_link_libraries(decal_draws_test PRIVATE bf6_core_static)
    target_include_directories(decal_draws_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(decal_draws_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(terrain_mesh_test test/terrain_mesh_test.cpp)
    target_link_libraries(terrain_mesh_test PRIVATE bf6_core_static)
    target_include_directories(terrain_mesh_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(terrain_mesh_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(water_tree_test test/water_tree_test.cpp)
    target_link_libraries(water_tree_test PRIVATE bf6_core_static)
    target_include_directories(water_tree_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(water_tree_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(water_sea_state_test test/water_sea_state_test.cpp)
    target_link_libraries(water_sea_state_test PRIVATE bf6_core_static)
    target_include_directories(water_sea_state_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(water_sea_state_test PRIVATE xaudio2 ole32)
    endif()
    add_executable(mount_snapshot_test test/mount_snapshot_test.cpp)
    target_link_libraries(mount_snapshot_test PRIVATE bf6_core_static)
    target_include_directories(mount_snapshot_test PRIVATE src include)
    if(WIN32)
        target_link_libraries(mount_snapshot_test PRIVATE xaudio2 ole32)
    endif()
endif()

option(BF6_BUILD_TOOLS "Build tests, probes and research tools" OFF)
if(NOT BF6_BUILD_TOOLS)
    return()
endif()

# Validation harness: reads one CAS reference so its output can be byte-compared
# to the working Godot plugin. Not shipped.
add_executable(cas_test test/cas_test.cpp)
target_link_libraries(cas_test PRIVATE bf6_core_static)
target_include_directories(cas_test PRIVATE src include)

add_executable(cas_range_test test/cas_range_test.cpp)
target_link_libraries(cas_range_test PRIVATE bf6_core_static)
target_include_directories(cas_range_test PRIVATE src include)
add_executable(bcn_decoder_equivalence_test test/bcn_decoder_equivalence_test.cpp)
target_link_libraries(bcn_decoder_equivalence_test PRIVATE bf6_core_static)
target_include_directories(bcn_decoder_equivalence_test PRIVATE src include)
add_executable(partition_stream_test test/partition_stream_test.cpp)
target_link_libraries(partition_stream_test PRIVATE bf6_core_static)
target_include_directories(partition_stream_test PRIVATE src include)
add_executable(terrain_sampled_test test/terrain_sampled_test.cpp)
target_link_libraries(terrain_sampled_test PRIVATE bf6_core_static)
target_include_directories(terrain_sampled_test PRIVATE src include)
add_executable(mount_identity_test test/mount_identity_test.cpp)
target_link_libraries(mount_identity_test PRIVATE bf6_core_static)
target_include_directories(mount_identity_test PRIVATE src include)

# Validates cas_path resolution against the plugin.
add_executable(loc_test test/loc_test.cpp)
target_link_libraries(loc_test PRIVATE bf6_core_static)
target_include_directories(loc_test PRIVATE src include)

# Module 4: dumps type layouts so they can be compared to typesdk.py, the
# Python reference the GDScript reader was itself ported from.
add_executable(types_test test/types_test.cpp)
target_link_libraries(types_test PRIVATE bf6_core_static)
target_include_directories(types_test PRIVATE src include)

# The whole material chain end to end: section -> state key -> bundle -> depot
# -> slots -> guids -> asset names -> pixels. Reports the survival rate at every
# link, because a break anywhere gives an untextured prop and no error.
add_executable(material_test test/material_test.cpp)
target_link_libraries(material_test PRIVATE bf6_core_static)

add_executable(material_parent_scope_test test/material_parent_scope_test.cpp)
target_link_libraries(material_parent_scope_test PRIVATE bf6_core_static)
target_include_directories(material_parent_scope_test PRIVATE src include)

add_executable(hangar_material_audit test/hangar_material_audit.cpp)
target_link_libraries(hangar_material_audit PRIVATE bf6_core_static)
target_include_directories(hangar_material_audit PRIVATE src include)

add_executable(vista_test test/vista_test.cpp)
target_link_libraries(vista_test PRIVATE bf6_core_static)

add_executable(water_test test/water_test.cpp)
target_link_libraries(water_test PRIVATE bf6_core_static)

add_executable(watercomposite_test test/watercomposite_test.cpp)
target_include_directories(watercomposite_test PRIVATE src include)
target_link_libraries(watercomposite_test PRIVATE bf6_core_static)

add_executable(watermask_runtime_test test/watermask_runtime_test.cpp)
target_include_directories(watermask_runtime_test PRIVATE src include)
target_link_libraries(watermask_runtime_test PRIVATE bf6_core_static)

add_executable(whitecards_test test/whitecards_test.cpp)
target_include_directories(whitecards_test PRIVATE src include)

add_executable(splat_test test/splat_test.cpp)
target_include_directories(splat_test PRIVATE src include)
target_link_libraries(splat_test PRIVATE bf6_core_static)

add_executable(watermask_probe probes/watermask_probe.cpp)
target_include_directories(watermask_probe PRIVATE src include)
target_link_libraries(watermask_probe PRIVATE bf6_core_static)

add_executable(utilityraster_probe probes/utilityraster_probe.cpp)
target_include_directories(utilityraster_probe PRIVATE src include)
target_link_libraries(utilityraster_probe PRIVATE bf6_core_static)

add_executable(tilescan_test test/tilescan_test.cpp)
target_include_directories(tilescan_test PRIVATE src include)
target_link_libraries(tilescan_test PRIVATE bf6_core_static)

add_executable(terrainlayers_test test/terrainlayers_test.cpp)
target_include_directories(terrainlayers_test PRIVATE src include)
target_link_libraries(terrainlayers_test PRIVATE bf6_core_static)

add_executable(groundcov_test test/groundcov_test.cpp)
target_include_directories(groundcov_test PRIVATE src include)
target_link_libraries(groundcov_test PRIVATE bf6_core_static)
add_executable(terrainbounds_live_test test/terrainbounds_live_test.cpp)
target_include_directories(terrainbounds_live_test PRIVATE src include)
target_link_libraries(terrainbounds_live_test PRIVATE bf6_core_static)
add_executable(terrainwindow_test test/terrainwindow_test.cpp)
target_include_directories(terrainwindow_test PRIVATE src include)
target_link_libraries(terrainwindow_test PRIVATE bf6_core_static)
add_executable(terrainmask_test test/terrainmask_test.cpp)
target_include_directories(terrainmask_test PRIVATE src include)
target_link_libraries(terrainmask_test PRIVATE bf6_core_static)
add_executable(terrainbindless_test test/terrainbindless_test.cpp)
target_include_directories(terrainbindless_test PRIVATE src include)
target_link_libraries(terrainbindless_test PRIVATE bf6_core_static)
add_executable(terrainpage_test test/terrainpage_test.cpp)
target_include_directories(terrainpage_test PRIVATE src include)
target_link_libraries(terrainpage_test PRIVATE bf6_core_static)

add_executable(scatter_test test/scatter_test.cpp)
target_include_directories(scatter_test PRIVATE src include)
target_link_libraries(scatter_test PRIVATE bf6_core_static)

add_executable(waterdepth_test test/waterdepth_test.cpp)
target_include_directories(waterdepth_test PRIVATE src include)
target_link_libraries(waterdepth_test PRIVATE bf6_core_static)

add_executable(waterheightfield_test test/waterheightfield_test.cpp)
target_include_directories(waterheightfield_test PRIVATE src include)
target_link_libraries(waterheightfield_test PRIVATE bf6_core_static)

add_executable(terraincomposite_test test/terraincomposite_test.cpp)
target_include_directories(terraincomposite_test PRIVATE src include)
target_link_libraries(terraincomposite_test PRIVATE bf6_core_static)
add_executable(terrainstatic_test test/terrainstatic_test.cpp)
target_include_directories(terrainstatic_test PRIVATE src include)
target_link_libraries(terrainstatic_test PRIVATE bf6_core_static)

add_executable(terrainstatic_live_test test/terrainstatic_live_test.cpp)
target_include_directories(terrainstatic_live_test PRIVATE src include)
target_link_libraries(terrainstatic_live_test PRIVATE bf6_core_static)

# Research proof: reads the live map-specific terrain ComputeLayer bytecode and
# validates its exact D3D12 binding contract. It never consumes a dumped shader.
if(WIN32)
  # Optional 2026 DirectX Dump Files capability probe.  The Agility SDK is an
  # explicit configure-time input so normal libbf6 builds do not depend on a
  # machine-local preview SDK.
  set(BF6_DXDMP_AGILITY_DIR "" CACHE PATH
      "Extracted Microsoft.Direct3D.D3D12 preview NuGet package")
  if(BF6_DXDMP_AGILITY_DIR)
    set(BF6_DXDMP_NATIVE "${BF6_DXDMP_AGILITY_DIR}/build/native")
    add_executable(dxdmp_probe test/dxdmp_probe.cpp)
    target_include_directories(dxdmp_probe BEFORE PRIVATE
      "${BF6_DXDMP_NATIVE}/include")
    target_link_libraries(dxdmp_probe PRIVATE d3d12 dxgi advapi32)
    add_custom_command(TARGET dxdmp_probe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "$<TARGET_FILE_DIR:dxdmp_probe>/D3D12"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${BF6_DXDMP_NATIVE}/bin/x64/D3D12Core.dll"
              "$<TARGET_FILE_DIR:dxdmp_probe>/D3D12/D3D12Core.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${BF6_DXDMP_NATIVE}/bin/x64/d3d12SDKLayers.dll"
              "$<TARGET_FILE_DIR:dxdmp_probe>/D3D12/d3d12SDKLayers.dll")
  endif()

  add_executable(terraindxil_test test/terraindxil_test.cpp)
  target_include_directories(terraindxil_test PRIVATE src include)
  target_link_libraries(terraindxil_test PRIVATE bf6_core_static d3d12 dxgi)
  add_executable(terraindxil_dispatch_test test/terraindxil_dispatch_test.cpp)
  target_include_directories(terraindxil_dispatch_test PRIVATE src include)
  target_link_libraries(terraindxil_dispatch_test PRIVATE bf6_core_static d3d12 dxgi)
  add_executable(terrainrows_test test/terrainrows_test.cpp)
  target_include_directories(terrainrows_test PRIVATE src include)
  target_link_libraries(terrainrows_test PRIVATE bf6_core_static)
  # Existing research-hub evidence dumper. Its .dxbc/.json outputs are
  # disassembly evidence only and are never runtime inputs to libbf6.
  # Built only beside a research checkout that has the source.
  set(BF6_TKDUMP_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../BF6_Frostbite_Research/impl/terrain_static_join/tkdump_test.cpp)
  if(EXISTS ${BF6_TKDUMP_SOURCE})
    add_executable(terrainkernel_dump_test ${BF6_TKDUMP_SOURCE})
    target_include_directories(terrainkernel_dump_test PRIVATE src include)
    target_link_libraries(terrainkernel_dump_test PRIVATE bf6_core_static)
  endif()
endif()

add_executable(bindingset_test test/bindingset_test.cpp)
target_include_directories(bindingset_test PRIVATE src include)
target_link_libraries(bindingset_test PRIVATE bf6_core_static)

add_executable(terrainsurface_raster_test test/terrainsurface_raster_test.cpp)
target_include_directories(terrainsurface_raster_test PRIVATE src include)
target_link_libraries(terrainsurface_raster_test PRIVATE bf6_core_static)

add_executable(kernelcensus_test test/kernelcensus_test.cpp)
target_include_directories(kernelcensus_test PRIVATE src include)
target_link_libraries(kernelcensus_test PRIVATE bf6_core_static)

add_executable(findocean_test test/findocean_test.cpp)
target_include_directories(findocean_test PRIVATE src include)
target_link_libraries(findocean_test PRIVATE bf6_core_static)

add_executable(oceankernel_test test/oceankernel_test.cpp)
target_include_directories(oceankernel_test PRIVATE src include)
target_link_libraries(oceankernel_test PRIVATE bf6_core_static)

add_executable(watershaders_test test/watershaders_test.cpp)
target_include_directories(watershaders_test PRIVATE src include)
target_link_libraries(watershaders_test PRIVATE bf6_core_static)

add_executable(oceanshader_test test/oceanshader_test.cpp)
target_include_directories(oceanshader_test PRIVATE src include)
target_link_libraries(oceanshader_test PRIVATE bf6_core_static)

add_executable(variation_test test/variation_test.cpp)
target_include_directories(variation_test PRIVATE src include)
target_link_libraries(variation_test PRIVATE bf6_core_static)
target_link_libraries(whitecards_test PRIVATE bf6_core_static)
target_include_directories(material_test PRIVATE src include)

# Research: names the depot's texture slots from the data, by resolving what is
# bound to each and counting the asset-name suffixes.
add_executable(slots_test test/slots_test.cpp)
target_link_libraries(slots_test PRIVATE bf6_core_static)
target_include_directories(slots_test PRIVATE src include)

# Module 8: the depot join. The format is unusually self-checking - records must
# tile the blob area and blobs must be consumed to the byte - so a wrong parse
# cannot pass quietly.
add_executable(depot_test test/depot_test.cpp)
target_link_libraries(depot_test PRIVATE bf6_core_static)
target_include_directories(depot_test PRIVATE src include)

# Module 7: decodes textures and reports the things that go plausibly wrong -
# a transposed non-square image, a chunk read short, an sRGB flag on linear data.
add_executable(texture_test test/texture_test.cpp)
target_link_libraries(texture_test PRIVATE bf6_core_static)
target_include_directories(texture_test PRIVATE src include)

# Module 10 (heights): reads a level's heightfield and reports the tree it came
# from, so a wrong stride shows up as a bad shape rather than as bad pixels.
add_executable(terrain_test test/terrain_test.cpp)
add_executable(terrainfit_test test/terrainfit_test.cpp)
target_link_libraries(terrainfit_test PRIVATE bf6_core_static)
target_include_directories(terrainfit_test PRIVATE src include)
target_link_libraries(terrain_test PRIVATE bf6_core_static)
target_include_directories(terrain_test PRIVATE src include)

# Module 9: the placement walk, dumped so it can be compared row for row with
# the shipping Godot plugin's own cached walk of the same map.
add_executable(walk_test test/walk_test.cpp)
target_link_libraries(walk_test PRIVATE bf6_core_static)
target_include_directories(walk_test PRIVATE src include)

# Level mounting: the order is the correctness, so it is printed and checked.
add_executable(level_test test/level_test.cpp)
target_link_libraries(level_test PRIVATE bf6_core_static)
target_include_directories(level_test PRIVATE src include)

# Module 5: pulls a real partition out of the install and flattens the decode,
# to be compared line for line with the Python reference.
add_executable(ebx_test test/ebx_test.cpp)
target_link_libraries(ebx_test PRIVATE bf6_core_static)
target_include_directories(ebx_test PRIVATE src include)

# Validates the SuperBundle TOC parse against the plugin.
add_executable(toc_test test/toc_test.cpp)
target_link_libraries(toc_test PRIVATE bf6_core_static)
target_include_directories(toc_test PRIVATE src include)

# Validates the full mount: name -> decompressed bytes.
add_executable(source_test test/source_test.cpp)
target_link_libraries(source_test PRIVATE bf6_core_static)
target_include_directories(source_test PRIVATE src include)

add_executable(meshset_test test/meshset_test.cpp)
target_link_libraries(meshset_test PRIVATE bf6_core_static)
target_include_directories(meshset_test PRIVATE src include)

add_executable(skeleton_test test/skeleton_test.cpp)
target_link_libraries(skeleton_test PRIVATE bf6_core_static)
target_include_directories(skeleton_test PRIVATE src include)

add_executable(psdapply_test test/psdapply_test.cpp)
target_link_libraries(psdapply_test PRIVATE bf6_core_static)
target_include_directories(psdapply_test PRIVATE src include)

add_executable(character_test test/character_test.cpp)
target_link_libraries(character_test PRIVATE bf6_core_static)
target_include_directories(character_test PRIVATE src include)

add_executable(rid_test test/rid_test.cpp)
target_link_libraries(rid_test PRIVATE bf6_core_static)
target_include_directories(rid_test PRIVATE src include)

add_executable(psd_test test/psd_test.cpp)
target_link_libraries(psd_test PRIVATE bf6_core_static)
target_include_directories(psd_test PRIVATE src include)

add_executable(dofset_test test/dofset_test.cpp)
target_link_libraries(dofset_test PRIVATE bf6_core_static)
target_include_directories(dofset_test PRIVATE src include)

add_executable(anim_test test/anim_test.cpp)
target_link_libraries(anim_test PRIVATE bf6_core_static)
target_include_directories(anim_test PRIVATE src include)

add_executable(soldier_1p_clip_test test/soldier_1p_clip_test.cpp)
target_link_libraries(soldier_1p_clip_test PRIVATE bf6_core_static)
target_include_directories(soldier_1p_clip_test PRIVATE src include)

add_executable(ant_graph_test test/ant_graph_test.cpp)
target_link_libraries(ant_graph_test PRIVATE bf6_core_static)
target_include_directories(ant_graph_test PRIVATE src include)

add_executable(ant_cdb_replay_test test/ant_cdb_replay_test.cpp)
target_link_libraries(ant_cdb_replay_test PRIVATE bf6_core_static)
target_include_directories(ant_cdb_replay_test PRIVATE src include)

add_executable(array_elem_slot_probe test/array_elem_slot_probe.cpp)
target_link_libraries(array_elem_slot_probe PRIVATE bf6_core_static)
target_include_directories(array_elem_slot_probe PRIVATE src include)

add_executable(cdb_probe test/cdb_probe.cpp)
target_link_libraries(cdb_probe PRIVATE bf6_core_static)
target_include_directories(cdb_probe PRIVATE src include)

add_executable(field_type_probe test/field_type_probe.cpp)
target_link_libraries(field_type_probe PRIVATE bf6_core_static)
target_include_directories(field_type_probe PRIVATE src include)

add_executable(anim_refgraph_probe test/anim_refgraph_probe.cpp)
target_link_libraries(anim_refgraph_probe PRIVATE bf6_core_static)
target_include_directories(anim_refgraph_probe PRIVATE src include)

add_executable(anim_1p_set_probe test/anim_1p_set_probe.cpp)
target_link_libraries(anim_1p_set_probe PRIVATE bf6_core_static)
target_include_directories(anim_1p_set_probe PRIVATE src include)

add_executable(anim_raw_probe test/anim_raw_probe.cpp)
target_link_libraries(anim_raw_probe PRIVATE bf6_core_static)
target_include_directories(anim_raw_probe PRIVATE src include)

add_executable(anim_vbr_live_test test/anim_vbr_live_test.cpp)
target_link_libraries(anim_vbr_live_test PRIVATE bf6_core_static)
target_include_directories(anim_vbr_live_test PRIVATE src include)

add_executable(anim_hpose_live_test test/anim_hpose_live_test.cpp)
target_link_libraries(anim_hpose_live_test PRIVATE bf6_core_static)
target_include_directories(anim_hpose_live_test PRIVATE src include)

add_executable(anim_binding_live_test test/anim_binding_live_test.cpp)
target_link_libraries(anim_binding_live_test PRIVATE bf6_core_static)

add_executable(anim_binding_route_probe test/anim_binding_route_probe.cpp)
target_link_libraries(anim_binding_route_probe PRIVATE bf6_core_static)
target_include_directories(anim_binding_live_test PRIVATE src include)

# Measures the current-install RigAsset quaternion/default basis against the
# render SkeletonAsset. This is a diagnostic gate for enabling frontend actor
# animation: it enumerates component/sign/transposition controls rather than
# selecting a visually plausible convention.
add_executable(anim_pose_basis_test test/anim_pose_basis_test.cpp)
target_link_libraries(anim_pose_basis_test PRIVATE bf6_core_static)
target_include_directories(anim_pose_basis_test PRIVATE src include)

# Keeps VBR's required post-IDCT quaternion normalization distinct from the
# DCT decoder's pre-normalization magnitude health check, then exercises the
# frontend model-rotation/local-translation pose convention.
add_executable(anim_vbr_pose_invariant_test test/anim_vbr_pose_invariant_test.cpp)
target_link_libraries(anim_vbr_pose_invariant_test PRIVATE bf6_core_static)
target_include_directories(anim_vbr_pose_invariant_test PRIVATE src include)

add_executable(chardeform_test test/chardeform_test.cpp)
target_link_libraries(chardeform_test PRIVATE bf6_core_static)
target_include_directories(chardeform_test PRIVATE src include)

add_executable(pose_test test/pose_test.cpp)
target_link_libraries(pose_test PRIVATE bf6_core_static)
target_include_directories(pose_test PRIVATE src include)

add_executable(skin_test test/skin_test.cpp)
target_link_libraries(skin_test PRIVATE bf6_core_static)
target_include_directories(skin_test PRIVATE src include)

add_executable(mesh_read_test test/mesh_read_test.cpp)
target_link_libraries(mesh_read_test PRIVATE bf6_core_static)
target_include_directories(mesh_read_test PRIVATE src include)

# Validates the SDK placeable catalogue + per-level filter against the counts we
# computed straight from the JSON (universal 1516; e.g. MP_Dumbo total 3015).
add_executable(placeables_test test/placeables_test.cpp)
target_link_libraries(placeables_test PRIVATE bf6_core_static)
target_include_directories(placeables_test PRIVATE src include)
add_executable(albedo_test test/albedo_test.cpp)
target_link_libraries(albedo_test PRIVATE bf6_core_static)
target_include_directories(albedo_test PRIVATE src include)
add_executable(destruction_test test/destruction_test.cpp)
target_link_libraries(destruction_test PRIVATE bf6_core_static)
target_include_directories(destruction_test PRIVATE src include)
add_executable(pipeline_test test/pipeline_test.cpp)
target_link_libraries(pipeline_test PRIVATE bf6_core_static)
target_include_directories(pipeline_test PRIVATE src include)
add_executable(decals_test test/decals_test.cpp)
target_link_libraries(decals_test PRIVATE bf6_core_static)
target_include_directories(decals_test PRIVATE src include)

add_executable(decaltint_test test/decaltint_test.cpp)
target_link_libraries(decaltint_test PRIVATE bf6_core_static)
target_include_directories(decaltint_test PRIVATE src include)

add_executable(decalwhite_test test/decalwhite_test.cpp)
target_link_libraries(decalwhite_test PRIVATE bf6_core_static)
target_include_directories(decalwhite_test PRIVATE src include)

add_executable(vicinity_test test/vicinity_test.cpp)
target_link_libraries(vicinity_test PRIVATE bf6_core_static)
target_include_directories(vicinity_test PRIVATE src include)

add_executable(decaltintmul_test test/decaltintmul_test.cpp)
target_link_libraries(decaltintmul_test PRIVATE bf6_core_static)
target_include_directories(decaltintmul_test PRIVATE src include)

# The VisualEnvironment decode through the C ABI, checked against the four rows
# of the plugin's hand-built lighting table.
add_executable(velight_test test/velight_test.cpp)
target_link_libraries(velight_test PRIVATE bf6_core_static)
target_include_directories(velight_test PRIVATE src include)

# Research: the raw VisualEnvironment dump the lighting constants were read off.
# Emits (component type_guid, field hash, value) triples for a join against
# data/sdk_field_names.tsv, because the hash is not reversible.
# The local light placements: the count, the placement split (which is where
# the classic bug hides), the distributions and a sample in real units.
add_executable(levellights_test test/levellights_test.cpp)
target_link_libraries(levellights_test PRIVATE bf6_core_static)
target_include_directories(levellights_test PRIVATE src include)

add_executable(reflection_test test/reflection_test.cpp)
target_link_libraries(reflection_test PRIVATE bf6_core_static)
target_include_directories(reflection_test PRIVATE src include)

add_executable(import_backref_test test/import_backref_test.cpp)
target_link_libraries(import_backref_test PRIVATE bf6_core_static)
target_include_directories(import_backref_test PRIVATE src include)

# Exact local VisualEnvironment trigger geometry from the installed level.
# Reports the real AreaProximity->shape joins beside a rotated-source control.
add_executable(lightingzones_test test/lightingzones_test.cpp)
target_link_libraries(lightingzones_test PRIVATE bf6_core_static)
target_include_directories(lightingzones_test PRIVATE src include)

add_executable(vedump_test test/vedump_test.cpp)
target_link_libraries(vedump_test PRIVATE bf6_core_static)
target_include_directories(vedump_test PRIVATE src include)

# Research: the authored water constants - the VE ocean component's fields
# beside the water material's depot record, so the pair can be solved together.
add_executable(waterconst_test test/waterconst_test.cpp)
target_link_libraries(waterconst_test PRIVATE bf6_core_static)
target_include_directories(waterconst_test PRIVATE src include)

add_executable(waterclosure_test test/waterclosure_test.cpp)
target_link_libraries(waterclosure_test PRIVATE bf6_core_static)
target_include_directories(waterclosure_test PRIVATE src include)

# Runs the current installed ocean DXIL directly against live H0. Research
# oracle only: no dumped shader or staged spectrum is a runtime input.
add_executable(waterdxil_test test/waterdxil_test.cpp)
target_link_libraries(waterdxil_test PRIVATE bf6_core_static d3d12 dxgi)
target_include_directories(waterdxil_test PRIVATE src include)

# Creates and executes the current installed level's selected water graphics
# PSO against a small offscreen synthetic-neutral scene.  This is the raster
# counterpart to waterdxil_test; it consumes no staged shader or texture dump.
add_executable(waterdraw_test test/waterdraw_test.cpp)
target_link_libraries(waterdraw_test PRIVATE bf6_core_static d3d12 dxgi)
target_include_directories(waterdraw_test PRIVATE src include)

add_executable(waterbind_test test/waterbind_test.cpp)
target_link_libraries(waterbind_test PRIVATE bf6_core_static)
target_include_directories(waterbind_test PRIVATE src include)

add_executable(bundlecount_test test/bundlecount_test.cpp)
target_link_libraries(bundlecount_test PRIVATE bf6_core_static)
target_include_directories(bundlecount_test PRIVATE src include)

add_executable(waterdepthcmp_test test/waterdepthcmp_test.cpp)
target_link_libraries(waterdepthcmp_test PRIVATE bf6_core_static)
target_include_directories(waterdepthcmp_test PRIVATE src include)

# Module FX: every placed emitter LAYER on a level, with its sheet, its grid,
# its lighting model and its merged parameter table. Checked against
# data/fx_level_inventory.tsv, which is its oracle.
add_executable(fx_test test/fx_test.cpp)
target_link_libraries(fx_test PRIVATE bf6_core_static)
target_include_directories(fx_test PRIVATE src include)

# The same decode over the C ABI, called the way a binding calls it.
add_executable(fxabi_test test/fxabi_test.cpp)
target_link_libraries(fxabi_test PRIVATE bf6_core_static)
target_include_directories(fxabi_test PRIVATE src include)

add_executable(weaponspike_test test/weaponspike_test.cpp)
target_link_libraries(weaponspike_test PRIVATE bf6_core_static)
target_include_directories(weaponspike_test PRIVATE src include)

add_executable(weapon_fit_test test/weapon_fit_test.cpp)
target_link_libraries(weapon_fit_test PRIVATE bf6_core_static)
target_include_directories(weapon_fit_test PRIVATE src include)

add_executable(attachment_pose_test test/attachment_pose_test.cpp)
target_link_libraries(attachment_pose_test PRIVATE bf6_core_static)
target_include_directories(attachment_pose_test PRIVATE src include)

add_executable(factory_fit_test test/factory_fit_test.cpp)
target_link_libraries(factory_fit_test PRIVATE bf6_core_static)
target_include_directories(factory_fit_test PRIVATE src include)

add_executable(package_identity_probe test/package_identity_probe.cpp)
target_link_libraries(package_identity_probe PRIVATE bf6_core_static)
target_include_directories(package_identity_probe PRIVATE src include)

add_executable(package_art_join_test test/package_art_join_test.cpp)
target_link_libraries(package_art_join_test PRIVATE bf6_core_static)
target_include_directories(package_art_join_test PRIVATE src include)

# Live package assembly coverage: all gameplay package records are read from
# equipment_<weapon>, then fitted through the model-definition graph.  Includes
# fake-weapon and unique-presentation controls.
add_executable(weapon_packages_test test/weapon_packages_test.cpp)
target_link_libraries(weapon_packages_test PRIVATE bf6_core_static)
target_include_directories(weapon_packages_test PRIVATE src include)

add_executable(field_upgrade_paths_test test/field_upgrade_paths_test.cpp)
target_link_libraries(field_upgrade_paths_test PRIVATE bf6_core_static)
target_include_directories(field_upgrade_paths_test PRIVATE src include)
if(BUILD_TESTING)
  add_test(NAME game.field_upgrade_paths COMMAND
    "$<TARGET_FILE:field_upgrade_paths_test>" "${BF6_TEST_GAME_DIR}")
  set_tests_properties(game.field_upgrade_paths PROPERTIES
    LABELS "game;ui;loadout;provider;control"
    WORKING_DIRECTORY "${BF6_TEST_OUTPUT_DIR}"
    TIMEOUT 300)
  if(NOT EXISTS "${BF6_TEST_GAME_DIR}")
    set_tests_properties(game.field_upgrade_paths PROPERTIES DISABLED TRUE)
  endif()
endif()

add_executable(vehicle_archetype_metadata_test
  test/vehicle_archetype_metadata_test.cpp)
target_link_libraries(vehicle_archetype_metadata_test PRIVATE bf6_core_static)
target_include_directories(vehicle_archetype_metadata_test PRIVATE src include)
if(BUILD_TESTING)
  add_test(NAME game.vehicle_archetype_metadata COMMAND
    "$<TARGET_FILE:vehicle_archetype_metadata_test>" "${BF6_TEST_GAME_DIR}")
  set_tests_properties(game.vehicle_archetype_metadata PROPERTIES
    LABELS "game;ui;vehicles;provider;control"
    WORKING_DIRECTORY "${BF6_TEST_OUTPUT_DIR}"
    TIMEOUT 300)
  if(NOT EXISTS "${BF6_TEST_GAME_DIR}")
    set_tests_properties(game.vehicle_archetype_metadata PROPERTIES
      DISABLED TRUE)
  endif()
endif()

add_executable(rime_image_layout_test test/rime_image_layout_test.cpp)
target_include_directories(rime_image_layout_test PRIVATE viewer)
if(BUILD_TESTING)
  add_test(NAME unit.rime_image_layout COMMAND
    "$<TARGET_FILE:rime_image_layout_test>")
  set_tests_properties(unit.rime_image_layout PROPERTIES
    LABELS "unit;ui;rime;image;layout;control")
endif()

add_executable(rime_text_layout_test test/rime_text_layout_test.cpp)
target_include_directories(rime_text_layout_test PRIVATE viewer)
if(BUILD_TESTING)
  add_test(NAME unit.rime_text_layout COMMAND
    "$<TARGET_FILE:rime_text_layout_test>")
  set_tests_properties(unit.rime_text_layout PROPERTIES
    LABELS "unit;ui;rime;text;layout;control")
endif()

add_executable(weapon_skin_material_test test/weapon_skin_material_test.cpp)
target_link_libraries(weapon_skin_material_test PRIVATE bf6_core_static)
target_include_directories(weapon_skin_material_test PRIVATE src include)

add_executable(bundle_scope_probe test/bundle_scope_probe.cpp)
target_link_libraries(bundle_scope_probe PRIVATE bf6_core_static)
target_include_directories(bundle_scope_probe PRIVATE src include)


add_executable(armory_test test/armory_test.cpp)
target_link_libraries(armory_test PRIVATE bf6_core_static)
target_include_directories(armory_test PRIVATE src include)

add_executable(ebx_instance_imports_test test/ebx_instance_imports_test.cpp)
target_link_libraries(ebx_instance_imports_test PRIVATE bf6_core_static)
target_include_directories(ebx_instance_imports_test PRIVATE src include)

add_executable(gadget_ui_metadata_test test/gadget_ui_metadata_test.cpp)
target_link_libraries(gadget_ui_metadata_test PRIVATE bf6_core_static)
target_include_directories(gadget_ui_metadata_test PRIVATE src include)

add_executable(melee_ui_metadata_test test/melee_ui_metadata_test.cpp)
target_link_libraries(melee_ui_metadata_test PRIVATE bf6_core_static)
target_include_directories(melee_ui_metadata_test PRIVATE src include)

add_executable(armory_camera_composition_test
  test/armory_camera_composition_test.cpp
  viewer/armory_camera_composition.cpp)
target_link_libraries(armory_camera_composition_test PRIVATE bf6_core_static)
target_include_directories(armory_camera_composition_test PRIVATE src include viewer)

add_executable(armory_camera_math_test
  test/armory_camera_math_test.cpp
  viewer/armory_camera_composition.cpp)
target_link_libraries(armory_camera_math_test PRIVATE bf6_core_static)
target_include_directories(armory_camera_math_test PRIVATE include viewer)

# Supported update-time exporter: reads both the installed game's ModBuilder
# records and the current Portal SDK declarations. It never consumes staged
# armory JSON/TSV data, so a game or SDK update cannot leave a plausible stale
# mapping beside the executable.
add_executable(portal_armory_codegen tools/portal_armory_codegen.cpp)
target_link_libraries(portal_armory_codegen PRIVATE bf6_core_static)
target_include_directories(portal_armory_codegen PRIVATE src include)

# One-mount UI runtime coverage harness.  It composes the current-install Rime
# readers with the renderer's data-flow interpreter, but never consumes a
# generated TSV/JSON as runtime input.  JSON and SVG are outputs for humans and
# CI; they are not inputs to the viewer.
add_executable(ui_runtime_audit tools/ui_runtime_audit.cpp viewer/rime.cpp)
target_link_libraries(ui_runtime_audit PRIVATE bf6_core_static)
target_include_directories(ui_runtime_audit PRIVATE src include viewer)

# Fail-closed selected-screen gate for the compositor semantics that still
# prevent a pixel-exact Rime frame. Reads only the mounted current install;
# emitted JSON is diagnostic output and never a viewer input.
add_executable(ui_compositor_gate tools/ui_compositor_gate.cpp viewer/rime.cpp)
target_link_libraries(ui_compositor_gate PRIVATE bf6_core_static)
target_include_directories(ui_compositor_gate PRIVATE src include viewer)

# Direct-install animation census. This is a diagnostic/runtime-read tool: it
# never consumes the corpus tables and scores an absent-prefix control beside
# the selected Home/menu family.
add_executable(ui_animation_inventory tools/ui_animation_inventory.cpp)
target_link_libraries(ui_animation_inventory PRIVATE bf6_core_static)
target_include_directories(ui_animation_inventory PRIVATE include)

add_executable(ebxls_test test/ebxls_test.cpp)
target_link_libraries(ebxls_test PRIVATE bf6_core_static)
target_include_directories(ebxls_test PRIVATE src include)

add_executable(ebx_array_descriptor_probe test/ebx_array_descriptor_probe.cpp)
target_link_libraries(ebx_array_descriptor_probe PRIVATE bf6_core_static)
target_include_directories(ebx_array_descriptor_probe PRIVATE src include)

add_executable(rime_live_test test/rime_live_test.cpp)
target_link_libraries(rime_live_test PRIVATE bf6_core_static)
target_include_directories(rime_live_test PRIVATE src include)

add_executable(rime_timeline_live_test test/rime_timeline_live_test.cpp)
target_link_libraries(rime_timeline_live_test PRIVATE bf6_core_static)
target_include_directories(rime_timeline_live_test PRIVATE src include)

add_executable(rime_layout_track_live_test test/rime_layout_track_live_test.cpp)
target_link_libraries(rime_layout_track_live_test PRIVATE bf6_core_static)
target_include_directories(rime_layout_track_live_test PRIVATE src include)

add_executable(compare_bool_runtime_live_test
  test/compare_bool_runtime_live_test.cpp)
target_link_libraries(compare_bool_runtime_live_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(compare_bool_runtime_live_test PRIVATE
  include ui_runtime)

add_executable(layout_timeline_runtime_live_test
  test/layout_timeline_runtime_live_test.cpp)
target_link_libraries(layout_timeline_runtime_live_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(layout_timeline_runtime_live_test PRIVATE
  include ui_runtime)

add_executable(home_timeline_probe test/home_timeline_probe.cpp)
target_link_libraries(home_timeline_probe PRIVATE bf6_core_static)
target_include_directories(home_timeline_probe PRIVATE src include)

add_executable(rime_float_interpolator_live_test
  test/rime_float_interpolator_live_test.cpp)
target_link_libraries(rime_float_interpolator_live_test PRIVATE bf6_core_static)
target_include_directories(rime_float_interpolator_live_test PRIVATE src include)

add_executable(rime_start_logo_color_live_test
  test/rime_start_logo_color_live_test.cpp)
target_link_libraries(rime_start_logo_color_live_test PRIVATE bf6_core_static)
target_include_directories(rime_start_logo_color_live_test PRIVATE src include)

# Focused startup/cache control: mounts only the current install's front-end
# families and times the live armory GUID index without launching a viewer.
add_executable(armory_index_cache_test test/armory_index_cache_test.cpp)
target_link_libraries(armory_index_cache_test PRIVATE bf6_core_static)
target_include_directories(armory_index_cache_test PRIVATE src include)

# Parses the current install's SerializedExpressionNodeGraph resources and
# reports real results beside corruption and wrong-resource controls. This is
# the first layer of the offline DiceExpression runtime; no staged corpus table
# is a runtime input.
add_executable(expression_test test/expression_test.cpp)
target_link_libraries(expression_test PRIVATE bf6_core_static)
target_include_directories(expression_test PRIVATE src include)

add_executable(expression_registry_test test/expression_registry_test.cpp)
target_link_libraries(expression_registry_test PRIVATE bf6_core_static)
target_include_directories(expression_registry_test PRIVATE src include)

add_executable(expression_vm_test test/expression_vm_test.cpp)
target_link_libraries(expression_vm_test PRIVATE bf6_core_static)
target_include_directories(expression_vm_test PRIVATE src include)

add_executable(expression_weapon_runtime_test
  test/expression_weapon_runtime_test.cpp)
target_link_libraries(expression_weapon_runtime_test PRIVATE bf6_core_static)
target_include_directories(expression_weapon_runtime_test PRIVATE src include)

# Direct-install proof for MenuWeapon's expression-authored header actions.
# The test follows localize-call slot producers and reports a fabricated SID
# control, so an adjacent favorites description cannot become a fourth button.


add_executable(rime_interface_color_test
  test/rime_interface_color_test.cpp)
target_link_libraries(rime_interface_color_test PRIVATE bf6_core_static)
target_include_directories(rime_interface_color_test PRIVATE src include)

add_executable(rime_reference_frame_test
  test/rime_reference_frame_test.cpp viewer/rime.cpp)
target_link_libraries(rime_reference_frame_test PRIVATE bf6_core_static)
target_include_directories(rime_reference_frame_test PRIVATE src include viewer)

# Applies occurrence-safe post-binding state to the native Rime value model.
# The adapter consumes in-memory state from a current-install evaluator; it
# never reads an exported UI table or route-specific mapping.



# One-mount proof of the complete authored front-end spine: boot, legal,
# universal menu, weapons and attachment editing. It discovers the screens
# from the current install and reports real property wiring beside a shuffled
# producer control; no generated route table is a runtime input.
add_executable(frontend_flow_test test/frontend_flow_test.cpp)
target_link_libraries(frontend_flow_test PRIVATE bf6_core_static)
target_include_directories(frontend_flow_test PRIVATE src include)

# Runtime-only BootFlow catalogue.  It keeps authored screen availability
# separate from provider-owned presentation order and carries exact-path plus
# mutated/fake controls beside the real current-install reads.
add_executable(boot_flow_runtime_test
  test/boot_flow_runtime_test.cpp ui_runtime/boot_flow.cpp)
target_link_libraries(boot_flow_runtime_test PRIVATE bf6_core_static)
target_include_directories(boot_flow_runtime_test PRIVATE src include ui_runtime)

add_library(bf6_rime_runtime STATIC ui_runtime/rime_runtime.cpp)
target_link_libraries(bf6_rime_runtime PUBLIC bf6_core_static)
target_include_directories(bf6_rime_runtime PUBLIC include ui_runtime)

# Pure text runtime shared by measurement and painting.  The gate covers the
# complete brace dialect and all markup tag families observed in the mounted
# English table, including nested styles and device-filtered input prompts.
add_executable(rime_text_runtime_test
  test/rime_text_runtime_test.cpp ui_runtime/rime_text_runtime.cpp)
target_include_directories(rime_text_runtime_test PRIVATE ui_runtime)

add_executable(rime_markup_svg_live_test test/rime_markup_svg_live_test.cpp)
target_link_libraries(rime_markup_svg_live_test PRIVATE bf6_core_static)
target_include_directories(rime_markup_svg_live_test PRIVATE include)

# Installed RimeFlipbookTextureAsset -> explicit UV table -> atlas texture.
# The focused gate includes irregular UVs and fake/wrong-type controls, then
# exercises the viewer adapter and its deterministic frame clock.
add_executable(rime_flipbook_live_test
  test/rime_flipbook_live_test.cpp viewer/rime.cpp)
target_link_libraries(rime_flipbook_live_test PRIVATE bf6_core_static)
target_include_directories(rime_flipbook_live_test PRIVATE include viewer)

add_executable(synthetic_texture_ids_test
  test/synthetic_texture_ids_test.cpp)
target_include_directories(synthetic_texture_ids_test PRIVATE viewer)
if(BUILD_TESTING)
  add_test(NAME unit.synthetic_texture_ids COMMAND
    "$<TARGET_FILE:synthetic_texture_ids_test>")
  set_tests_properties(unit.synthetic_texture_ids PROPERTIES
    LABELS "unit;ui;texture;cache;control")
endif()

# Exact MovieTexture2 -> raw WebM path.  The reader copies the current mount's
# in-memory chunk and validates type, declared size, WebM signature and a
# mutated GUID; no exported video is used as a runtime input.
add_executable(movie_texture_test
  test/movie_texture_test.cpp ui_runtime/movie_texture.cpp)
target_link_libraries(movie_texture_test PRIVATE
  bf6_core_static mfplat mfreadwrite mfuuid shlwapi ole32)
target_include_directories(movie_texture_test PRIVATE src include ui_runtime)

# Generic all-screen route state.  The pure gate exercises identity, focus,
# state and event rejection without a game dependency.  The install gate
# enumerates and lazily opens the current Steam install's complete screen set.
add_executable(ui_screen_router_test
  test/ui_screen_router_test.cpp ui_runtime/screen_router.cpp)
target_include_directories(ui_screen_router_test PRIVATE ui_runtime)

add_executable(ui_screen_router_install_test
  test/ui_screen_router_install_test.cpp
  ui_runtime/screen_router.cpp
  ui_runtime/install_screen_source.cpp
  viewer/rime.cpp
  viewer/rime_state_bridge.cpp)
target_link_libraries(ui_screen_router_install_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(ui_screen_router_install_test PRIVATE
  src include viewer ui_runtime)

# One-hop authored event dispatch keeps occurrence identity and source/target
# polarity exact. It reports interface outputs as provider handoffs without
# inventing evaluator order or a navigation destination.
add_executable(ui_authored_event_dispatch_test
  test/ui_authored_event_dispatch_test.cpp
  ui_runtime/authored_event_dispatch.cpp)
target_include_directories(ui_authored_event_dispatch_test PRIVATE ui_runtime)

# Generic all-screen controller-neutral input bridge. Both gates use the same
# source-polarity authored-event selector; the install gate additionally
# audits current bf6.exe names, DBD signatures and mapping assets directly.
add_executable(ui_control_runtime_test
  test/ui_control_runtime_test.cpp
  ui_runtime/ui_control_runtime.cpp
  ui_runtime/authored_event_dispatch.cpp
  ui_runtime/screen_router.cpp
  ui_runtime/install_screen_source.cpp
  viewer/rime.cpp
  viewer/rime_state_bridge.cpp)
target_link_libraries(ui_control_runtime_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(ui_control_runtime_test PRIVATE
  src include viewer ui_runtime)

add_executable(ui_control_runtime_install_test
  test/ui_control_runtime_install_test.cpp
  ui_runtime/ui_control_runtime.cpp
  ui_runtime/authored_event_dispatch.cpp
  ui_runtime/screen_router.cpp
  ui_runtime/install_screen_source.cpp
  viewer/rime.cpp
  viewer/rime_state_bridge.cpp)
target_link_libraries(ui_control_runtime_install_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(ui_control_runtime_install_test PRIVATE
  src include viewer ui_runtime)

# Focused current-install proof for the ConceptIdentifier -> authored default
# physical-action join.  The probe keeps numeric device codes raw; icon maps
# are a separate installed-data join and no host key table participates.
add_executable(ui_default_input_binding_probe
  test/ui_default_input_binding_probe.cpp
  ui_runtime/default_input_bindings.cpp)
target_link_libraries(ui_default_input_binding_probe PRIVATE bf6_core_static)
target_include_directories(ui_default_input_binding_probe PRIVATE
  src include ui_runtime)

# Current-install BootFlow completion proof. Movie EOS and authored skip
# sources converge on the same output; the next screen remains an explicit
# offline replace because the provider-selected destination is not serialized.
add_executable(ui_boot_route_install_test
  test/ui_boot_route_install_test.cpp
  ui_runtime/screen_router.cpp
  ui_runtime/install_screen_source.cpp
  viewer/rime.cpp
  viewer/rime_state_bridge.cpp)
target_link_libraries(ui_boot_route_install_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(ui_boot_route_install_test PRIVATE
  src include viewer ui_runtime)

# The start prompt's focus chrome is a shipped nested property graph.  This
# gate proves that real IsFocused state crosses the exact nested widget
# occurrences while a one-bit-mutated field and fabricated route do nothing.
# Line animation remains unchanged until the referenced logic prefabs are
# executed; encoding that boundary prevents a static state bridge from being
# mistaken for a complete Rime animation player.
add_executable(ui_start_button_focus_live_test
  test/ui_start_button_focus_live_test.cpp
  viewer/rime.cpp)
target_link_libraries(ui_start_button_focus_live_test PRIVATE bf6_core_static)
target_include_directories(ui_start_button_focus_live_test PRIVATE
  src include viewer)

# Logic prefab imports are executable Rime graph nodes, not visual children.
# This live gate proves the focus-border -> blink import and a fabricated-
# partition control before the viewer is allowed to inline that graph.
add_executable(rime_logic_reference_live_test
  test/rime_logic_reference_live_test.cpp)
target_link_libraries(rime_logic_reference_live_test PRIVATE bf6_core_static)

# General engine-neutral Rime runtime. This compiles the live screen/widget
# graph plus every occurrence-scoped logic prefab and executes only operators
# whose behavior has a controlled current-install proof.
add_executable(rime_runtime_install_test
  test/rime_runtime_install_test.cpp)
target_link_libraries(rime_runtime_install_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(rime_runtime_install_test PRIVATE
  src include ui_runtime)

add_executable(home_linear_animation_runtime_test
  test/home_linear_animation_runtime_test.cpp)
target_link_libraries(home_linear_animation_runtime_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(home_linear_animation_runtime_test PRIVATE
  include ui_runtime)

add_executable(eased_interpolator_runtime_test
  test/eased_interpolator_runtime_test.cpp)
target_link_libraries(eased_interpolator_runtime_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(eased_interpolator_runtime_test PRIVATE
  include ui_runtime)

add_executable(direct_screen_animation_apply_test
  test/direct_screen_animation_apply_test.cpp
  ui_runtime/install_screen_source.cpp
  viewer/rime.cpp
  viewer/rime_state_bridge.cpp)
target_link_libraries(direct_screen_animation_apply_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(direct_screen_animation_apply_test PRIVATE
  src include viewer ui_runtime)

add_executable(home_composed_animation_route_test
  test/home_composed_animation_route_test.cpp
  ui_runtime/install_screen_source.cpp
  viewer/rime.cpp
  viewer/rime_state_bridge.cpp)
target_link_libraries(home_composed_animation_route_test PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(home_composed_animation_route_test PRIVATE
  src include viewer ui_runtime)

# Diagnostic companions for the boot-route gate.  Both read the current
# mounted install directly: one prints the exact logo EOS/skip convergence,
# while the other checks the specialized Intro DBD vocabulary against
# completed/general/fabricated controls.
add_executable(boot_logo_transition_probe
  test/boot_logo_transition_probe.cpp
  ui_runtime/screen_router.cpp
  ui_runtime/install_screen_source.cpp
  viewer/rime.cpp
  viewer/rime_state_bridge.cpp)
target_link_libraries(boot_logo_transition_probe PRIVATE
  bf6_core_static bf6_rime_runtime)
target_include_directories(boot_logo_transition_probe PRIVATE
  src include viewer ui_runtime)

add_executable(boot_intro_state_probe
  test/boot_intro_state_probe.cpp)
target_link_libraries(boot_intro_state_probe PRIVATE bf6_core_static)
target_include_directories(boot_intro_state_probe PRIVATE include)

# Provider-to-3D presenter contract.  Concrete models are accepted only from
# explicit local state and exact current-mount identities; unresolved live
# provider values remain unresolved instead of being guessed from filenames.
add_executable(ui_provider_registry_test
  test/ui_provider_registry_test.cpp viewer/ui_provider_registry.cpp)
target_link_libraries(ui_provider_registry_test PRIVATE bf6_core_static)
target_include_directories(ui_provider_registry_test PRIVATE src include viewer)

add_executable(catalogue_test test/catalogue_test.cpp)
target_link_libraries(catalogue_test PRIVATE bf6_core_static)
target_include_directories(catalogue_test PRIVATE src include)

add_executable(rime_paint_test test/rime_paint_test.cpp viewer/rime.cpp)
target_link_libraries(rime_paint_test PRIVATE bf6_core_static)
target_include_directories(rime_paint_test PRIVATE src include viewer)

# bf6viewer uses the stock RGBA-packed ImGui vertices required by the stock
# DX11 backend's R8G8B8A8 COLOR declaration.  Keep derived palette colours
# correct and gate the raw packed lane order used by that input layout.
add_executable(viewer_packed_color_test test/viewer_packed_color_test.cpp)
target_include_directories(viewer_packed_color_test PRIVATE
  viewer third_party/imgui)

add_executable(loadout_class_icon_live_test
  test/loadout_class_icon_live_test.cpp)
target_link_libraries(loadout_class_icon_live_test PRIVATE bf6_core_static)
target_include_directories(loadout_class_icon_live_test PRIVATE include)

# Prove which LoadoutClassItemData fields reach the exact footer cell before
# the standalone viewer is allowed to materialize that provider-owned list.
# Each real route is scored beside the mounted shuffled-property control.
add_executable(loadout_class_cell_route_test
  test/loadout_class_cell_route_test.cpp
  viewer/rime_list_provider.cpp viewer/rime.cpp)
target_link_libraries(loadout_class_cell_route_test PRIVATE bf6_core_static)
target_include_directories(loadout_class_cell_route_test PRIVATE
  src include viewer)

# Exact runtime adapter/proof for MenuWeaponScreen's data-bound stack lists.
# It reads the mounted UI graph and DBD field ids; no exported roster or
# hand-authored list is a runtime input.
add_executable(rime_list_provider_test
  test/rime_list_provider_test.cpp viewer/rime_list_provider.cpp viewer/rime.cpp)
target_link_libraries(rime_list_provider_test PRIVATE bf6_core_static)
target_include_directories(rime_list_provider_test PRIVATE src include viewer)

# Explicit, typed stand-in for online account/profile services.  The provider
# validates the mounted DBD and the live graph route before adding a value;
# fake-route and changed-signature controls are part of its executable test.
add_executable(offline_profile_provider_test
  test/offline_profile_provider_test.cpp
  viewer/offline_profile_provider.cpp
  viewer/rime_list_provider.cpp
  viewer/rime.cpp)
target_link_libraries(offline_profile_provider_test PRIVATE bf6_core_static)
target_include_directories(offline_profile_provider_test PRIVATE src include viewer)

# Current-install home presentation plus a single allowlisted, credential-free
# EA status GET.  The executable policy rejects every interactive online action.
add_executable(read_only_online_content_test
  test/read_only_online_content_test.cpp
  viewer/read_only_online_content.cpp)
target_link_libraries(read_only_online_content_test PRIVATE bf6_core_static winhttp)
target_include_directories(read_only_online_content_test PRIVATE src include viewer)

# Exact read-only bridge to BF6's public presentation manifest/cache. It
# accepts only allowlisted EA CDN menu families, validates cached or online
# bytes against the manifest size, and keeps changed-host controls executable.
add_executable(live_menu_cache_test
  test/live_menu_cache_test.cpp
  viewer/live_menu_cache.cpp)
target_include_directories(live_menu_cache_test PRIVATE viewer)
if(WIN32)
  target_link_libraries(live_menu_cache_test PRIVATE winhttp)
endif()

# Runtime-only proof for DataLists whose authored ItemTemplate is empty.  The
# test resolves the unique visual cell from the installed DBD partition GUID
# and the candidate cell's EFIX Imports[]; it never consumes the research GUID
# table or a generated mapping.
add_executable(rime_dynamic_list_cell_test
  test/rime_dynamic_list_cell_test.cpp viewer/rime_list_provider.cpp viewer/rime.cpp)
target_link_libraries(rime_dynamic_list_cell_test PRIVATE bf6_core_static)
target_include_directories(rime_dynamic_list_cell_test PRIVATE src include viewer)

add_executable(home_ui_chain_test test/home_ui_chain_test.cpp)
target_link_libraries(home_ui_chain_test PRIVATE bf6_core_static)
target_include_directories(home_ui_chain_test PRIVATE src include viewer)
add_executable(home_template_route_proof_test
  test/home_template_route_proof_test.cpp)
target_link_libraries(home_template_route_proof_test PRIVATE bf6_core_static)
target_include_directories(home_template_route_proof_test PRIVATE src include)

# Proves the installed Home backdrop loader/interface/fader route and keeps
# the provider-owned BulletinImage boundary explicit.  A fabricated partition
# and shuffled property edge are evaluated beside the real graph.
add_executable(home_background_route_probe
  test/home_background_route_probe.cpp)
target_link_libraries(home_background_route_probe PRIVATE bf6_core_static)
target_include_directories(home_background_route_probe PRIVATE src include)

add_executable(offline_home_presenter_probe
    test/offline_home_presenter_probe.cpp
    viewer/offline_home_provider.cpp
    viewer/offline_home_presenter.cpp
    viewer/rime.cpp
    viewer/rime_list_provider.cpp)
target_link_libraries(offline_home_presenter_probe PRIVATE bf6_core_static)
target_include_directories(offline_home_presenter_probe PRIVATE src include viewer)

# BF6 ships an authored developer/offline Home provider.  This gate reads its
# record/collection graph from the current mount, joins every record against
# its mounted DBD, and runs fake-partition plus shuffled-field controls.  It is
# an offline presentation input, never a substitute for production services.
add_executable(offline_home_provider_test
  test/offline_home_provider_test.cpp
  viewer/offline_home_provider.cpp
  viewer/rime_list_provider.cpp
  viewer/rime.cpp)
target_link_libraries(offline_home_provider_test PRIVATE bf6_core_static)
target_include_directories(offline_home_provider_test PRIVATE src include viewer)

add_executable(universal_primary_provider_test
  test/universal_primary_provider_test.cpp
  viewer/universal_primary_provider.cpp
  viewer/rime.cpp)
target_link_libraries(universal_primary_provider_test PRIVATE bf6_core_static)
target_include_directories(universal_primary_provider_test PRIVATE src include viewer)

add_executable(localized_reverse_test test/localized_reverse_test.cpp)
target_link_libraries(localized_reverse_test PRIVATE bf6_core_static)
target_include_directories(localized_reverse_test PRIVATE src include)

add_executable(loadout_screen_probe test/loadout_screen_probe.cpp viewer/rime.cpp)
target_link_libraries(loadout_screen_probe PRIVATE bf6_core_static)
target_include_directories(loadout_screen_probe PRIVATE src include viewer)

add_executable(asset_owner_test test/asset_owner_test.cpp)
target_link_libraries(asset_owner_test PRIVATE bf6_core_static)
target_include_directories(asset_owner_test PRIVATE src include)

add_executable(asset_instances_test test/asset_instances_test.cpp)
target_link_libraries(asset_instances_test PRIVATE bf6_core_static)
target_include_directories(asset_instances_test PRIVATE src include)

add_executable(smg_live_test test/smg_live_test.cpp)
target_link_libraries(smg_live_test PRIVATE bf6_core_static)
target_include_directories(smg_live_test PRIVATE src include)

add_executable(loadout_class_camera_test test/loadout_class_camera_test.cpp)
target_link_libraries(loadout_class_camera_test PRIVATE bf6_core_static)
target_include_directories(loadout_class_camera_test PRIVATE src include)

# Resolves a live hardware MeshSet state key through the installed
# ExpressionShaderDatabase to its selected raster bytecode.  The paired fake
# key is a required control; output shaders are research artifacts only.
add_executable(hardware_shader_test test/hardware_shader_test.cpp)
target_link_libraries(hardware_shader_test PRIVATE bf6_core_static)
target_include_directories(hardware_shader_test PRIVATE src include)

# ---- native viewer: Dear ImGui (Win32 + D3D11) over libbf6 -------------------
# The shell is upstream's own example backend, unmodified. Only the BF6 half is
# ours, which is the point: no time spent rewriting a window and a device.
set(IMGUI third_party/imgui)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${IMGUI}/imgui.cpp)
  add_executable(bf6viewer
    viewer/bf6viewer.cpp
    ui_runtime/authored_event_dispatch.cpp
    ui_runtime/boot_flow.cpp
    ui_runtime/default_input_bindings.cpp
    ui_runtime/movie_texture.cpp
    ui_runtime/rime_text_runtime.cpp
    ui_runtime/screen_router.cpp
    ui_runtime/ui_control_runtime.cpp
    ui_runtime/install_screen_source.cpp
    ui_runtime/installed_sound_wave.cpp
    viewer/rime.cpp
    viewer/ui_provider_registry.cpp
    viewer/rime_state_bridge.cpp
    viewer/rime_state_process.cpp
    viewer/rime_list_provider.cpp
    viewer/offline_home_provider.cpp
    viewer/offline_home_presenter.cpp
    viewer/offline_profile_provider.cpp
    viewer/read_only_online_content.cpp
    viewer/live_menu_cache.cpp
    viewer/universal_primary_provider.cpp
    viewer/armory_header_actions.cpp
    viewer/armory_ui.cpp
    viewer/armory_camera_composition.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp)
  # The stock DX11 backend declares ImDrawVert::col as
  # DXGI_FORMAT_R8G8B8A8_UNORM.  Keep ImGui's stock RGBA packing as well.
  # Defining IMGUI_USE_BGRA_PACKED_COLOR while retaining that input layout
  # reverses R/B in every vector/label colour (the installed gold currency
  # icon becomes cyan).  Texture SRVs keep their own declared DXGI formats and
  # do not require changing the packed vertex-colour ABI.
  get_filename_component(BF6_WORKSPACE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
  file(TO_CMAKE_PATH
    "${BF6_WORKSPACE_ROOT}/BF6_Frostbite_Research/tools/current_install_ui_state_capture.py"
    BF6_UI_STATE_TOOL_PATH)
  target_compile_definitions(bf6viewer PRIVATE
    BF6_UI_STATE_TOOL_DEFAULT="${BF6_UI_STATE_TOOL_PATH}")
  # Keep optimized viewer symbols beside release builds.  UI composition is
  # deliberately data-driven and a bad runtime tree can otherwise leave only
  # an opaque executable RVA in Windows Error Reporting.
  if(MSVC)
    target_compile_options(bf6viewer PRIVATE /Zi)
    target_link_options(bf6viewer PRIVATE /DEBUG:FULL /MAP)
  endif()
  target_include_directories(bf6viewer PRIVATE
    src include viewer ui_runtime ${IMGUI} ${IMGUI}/backends)
  target_link_libraries(bf6viewer PRIVATE
    bf6_core_static bf6_rime_runtime d3d11 dxgi d3dcompiler
    mfplat mfreadwrite mfuuid shlwapi ole32 winhttp xaudio2)
  # Local viewer assets are optional and never part of the repository.
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/viewer/assets)
    add_custom_command(TARGET bf6viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/viewer/assets
              $<TARGET_FILE_DIR:bf6viewer>/assets)
  endif()
endif()

# CTest is an inventory and repeatable runner for the proof harnesses. The
# harness executables remain ordinary binaries; tests that need the installed
# game are registered but disabled when the Steam install is not configured.
include(CTest)
if(BUILD_TESTING)
  include(cmake/BF6Tests.cmake)
endif()
add_executable(dofclose_test test/dofclose_test.cpp)
target_link_libraries(dofclose_test PRIVATE bf6_core_static)
target_include_directories(dofclose_test PRIVATE src include)
add_executable(swarm_test test/swarm_test.cpp)
target_link_libraries(swarm_test PRIVATE bf6_core_static)
target_include_directories(swarm_test PRIVATE src include)
add_executable(clipinfo_test test/clipinfo_test.cpp)
target_link_libraries(clipinfo_test PRIVATE bf6_core_static)
target_include_directories(clipinfo_test PRIVATE src include)
add_executable(chanbone_test test/chanbone_test.cpp)
target_link_libraries(chanbone_test PRIVATE bf6_core_static)
target_include_directories(chanbone_test PRIVATE src include)
add_executable(telemetry_test test/telemetry_test.cpp)
target_link_libraries(telemetry_test PRIVATE bf6_core_static)
target_include_directories(telemetry_test PRIVATE src include)

add_executable(reflection_probes_test test/reflection_probes_test.cpp)
target_link_libraries(reflection_probes_test PRIVATE bf6_core_static)
target_include_directories(reflection_probes_test PRIVATE src include)

add_executable(fx_budget_test test/fx_budget_test.cpp)
target_link_libraries(fx_budget_test PRIVATE bf6_core_static)
target_include_directories(fx_budget_test PRIVATE src include)

add_executable(fx_param_dump test/fx_param_dump.cpp)
target_link_libraries(fx_param_dump PRIVATE bf6_core_static)
target_include_directories(fx_param_dump PRIVATE src include)

add_executable(water_cascades_probe test/water_cascades_probe.cpp)
target_link_libraries(water_cascades_probe PRIVATE bf6_core_static)
target_include_directories(water_cascades_probe PRIVATE src include)

add_executable(pose_owner_probe test/pose_owner_probe.cpp)
target_link_libraries(pose_owner_probe PRIVATE bf6_core_static)
target_include_directories(pose_owner_probe PRIVATE src include)

# Also defined in the BF6_BUILD_CORE_TESTS block above. With both options on,
# a second add_executable is a hard CMake error that blocks RECONFIGURING the
# whole build directory - which is how out-tools came to be silently stale
# against the levels/gr/ path change rather than merely out of date.
if(NOT TARGET soldier_skin_test)
    add_executable(soldier_skin_test test/soldier_skin_test.cpp)
    target_link_libraries(soldier_skin_test PRIVATE bf6_core_static)
    target_include_directories(soldier_skin_test PRIVATE src include)
endif()

add_executable(pose_clip_probe test/pose_clip_probe.cpp)
target_link_libraries(pose_clip_probe PRIVATE bf6_core_static)
target_include_directories(pose_clip_probe PRIVATE src include)

add_executable(fx_join_probe test/fx_join_probe.cpp)
target_link_libraries(fx_join_probe PRIVATE bf6_core_static)
target_include_directories(fx_join_probe PRIVATE src include)

add_executable(fx_look_test test/fx_look_test.cpp)
target_link_libraries(fx_look_test PRIVATE bf6_core_static)
target_include_directories(fx_look_test PRIVATE src include)

add_executable(anim_1p_probe test/anim_1p_probe.cpp)
target_link_libraries(anim_1p_probe PRIVATE bf6_core_static)
target_include_directories(anim_1p_probe PRIVATE src include)

add_executable(asset_count_probe test/asset_count_probe.cpp)
target_link_libraries(asset_count_probe PRIVATE bf6_core_static)
target_include_directories(asset_count_probe PRIVATE src include)

add_executable(sound_emitter_test test/sound_emitter_test.cpp)
target_link_libraries(sound_emitter_test PRIVATE bf6_core_static)
target_include_directories(sound_emitter_test PRIVATE src include)

add_executable(sound_emitter_probe test/sound_emitter_probe.cpp)
target_link_libraries(sound_emitter_probe PRIVATE bf6_core_static)
target_include_directories(sound_emitter_probe PRIVATE src include)

add_executable(water_override_test test/water_override_test.cpp)
target_link_libraries(water_override_test PRIVATE bf6_core_static)
target_include_directories(water_override_test PRIVATE src include)

add_executable(portal_carrier_test test/portal_carrier_test.cpp)
target_link_libraries(portal_carrier_test PRIVATE bf6_core_static)
target_include_directories(portal_carrier_test PRIVATE src include)

add_executable(carrier_placement_test test/carrier_placement_test.cpp)
target_link_libraries(carrier_placement_test PRIVATE bf6_core_static)
target_include_directories(carrier_placement_test PRIVATE src include)

add_executable(enlighten_key_test test/enlighten_key_test.cpp)
target_link_libraries(enlighten_key_test PRIVATE bf6_core_static)
target_include_directories(enlighten_key_test PRIVATE src include)

add_executable(reflection_scan_test test/reflection_scan_test.cpp)
target_link_libraries(reflection_scan_test PRIVATE bf6_core_static)
target_include_directories(reflection_scan_test PRIVATE src include)

add_executable(matgrid_test test/matgrid_test.cpp)
target_link_libraries(matgrid_test PRIVATE bf6_core_static)
target_include_directories(matgrid_test PRIVATE src include)

add_executable(material_pair_test test/material_pair_test.cpp)
target_link_libraries(material_pair_test PRIVATE bf6_core_static)
target_include_directories(material_pair_test PRIVATE src include)

add_executable(spawn_test test/spawn_test.cpp)
target_link_libraries(spawn_test PRIVATE bf6_core_static)
target_include_directories(spawn_test PRIVATE src include)

add_executable(vshape_test test/vshape_test.cpp)
target_link_libraries(vshape_test PRIVATE bf6_core_static)
target_include_directories(vshape_test PRIVATE src include)

add_executable(gamemode_test test/gamemode_test.cpp)
target_link_libraries(gamemode_test PRIVATE bf6_core_static)
target_include_directories(gamemode_test PRIVATE src include)

add_executable(footprint_test test/footprint_test.cpp)
target_link_libraries(footprint_test PRIVATE bf6_core_static)
target_include_directories(footprint_test PRIVATE src include)

add_executable(autopaint_test test/autopaint_test.cpp)
target_link_libraries(autopaint_test PRIVATE bf6_core_static)
target_include_directories(autopaint_test PRIVATE src include)

add_executable(debris_test test/debris_test.cpp)
target_link_libraries(debris_test PRIVATE bf6_core_static)
target_include_directories(debris_test PRIVATE src include)

add_executable(matrel_raw_test test/matrel_raw_test.cpp)
target_link_libraries(matrel_raw_test PRIVATE bf6_core_static)
target_include_directories(matrel_raw_test PRIVATE src include)

add_executable(wind_test test/wind_test.cpp)
target_link_libraries(wind_test PRIVATE bf6_core_static)
target_include_directories(wind_test PRIVATE src include)

add_executable(weather_test test/weather_test.cpp)
target_link_libraries(weather_test PRIVATE bf6_core_static)
target_include_directories(weather_test PRIVATE src include)

add_executable(wavesel_test test/wavesel_test.cpp)
target_link_libraries(wavesel_test PRIVATE bf6_core_static)
target_include_directories(wavesel_test PRIVATE src include)

add_executable(sps_test test/sps_test.cpp)
target_link_libraries(sps_test PRIVATE bf6_core_static)
target_include_directories(sps_test PRIVATE src include)

add_executable(spsdec_test test/spsdec_test.cpp)
target_link_libraries(spsdec_test PRIVATE bf6_core_static)
target_include_directories(spsdec_test PRIVATE src include)

add_executable(eal3_test test/eal3_test.cpp)
target_link_libraries(eal3_test PRIVATE bf6_core_static)
target_include_directories(eal3_test PRIVATE src include)

add_executable(eal3find_test test/eal3find_test.cpp)
target_link_libraries(eal3find_test PRIVATE bf6_core_static)
target_include_directories(eal3find_test PRIVATE src include)

add_executable(eal3walk_test test/eal3walk_test.cpp)
target_link_libraries(eal3walk_test PRIVATE bf6_core_static)
target_include_directories(eal3walk_test PRIVATE src include)

add_executable(eal3mp3_test test/eal3mp3_test.cpp)
target_link_libraries(eal3mp3_test PRIVATE bf6_core_static)
target_include_directories(eal3mp3_test PRIVATE src include)

add_executable(eal3v2_test test/eal3v2_test.cpp)
target_link_libraries(eal3v2_test PRIVATE bf6_core_static)
target_include_directories(eal3v2_test PRIVATE src include)

add_executable(eal3rebuild_test test/eal3rebuild_test.cpp)
target_link_libraries(eal3rebuild_test PRIVATE bf6_core_static)
target_include_directories(eal3rebuild_test PRIVATE src include)
add_executable(typescan_test test/typescan_test.cpp)
target_link_libraries(typescan_test PRIVATE bf6_core_static)
target_include_directories(typescan_test PRIVATE src include)
add_executable(gpcensus_test test/gpcensus_test.cpp)
target_link_libraries(gpcensus_test PRIVATE bf6_core_static)
target_include_directories(gpcensus_test PRIVATE src include)
add_executable(exedb_test test/exedb_test.cpp)
target_link_libraries(exedb_test PRIVATE bf6_core_static)
target_include_directories(exedb_test PRIVATE src include)
add_executable(gameplay_test test/gameplay_test.cpp)
target_link_libraries(gameplay_test PRIVATE bf6_core_static)
target_include_directories(gameplay_test PRIVATE src include)
add_executable(mbprobe_test test/mbprobe_test.cpp)
target_link_libraries(mbprobe_test PRIVATE bf6_core_static)
target_include_directories(mbprobe_test PRIVATE src include)
add_executable(physics_test test/physics_test.cpp)
target_link_libraries(physics_test PRIVATE bf6_core_static)
target_include_directories(physics_test PRIVATE src include)
add_executable(material_link_test test/material_link_test.cpp)
target_link_libraries(material_link_test PRIVATE bf6_core_static)
target_include_directories(material_link_test PRIVATE src include)
add_executable(placereach_test test/placereach_test.cpp)
target_link_libraries(placereach_test PRIVATE bf6_core_static)
target_include_directories(placereach_test PRIVATE src include)
add_executable(occluder_test test/occluder_test.cpp)
target_link_libraries(occluder_test PRIVATE bf6_core_static)
target_include_directories(occluder_test PRIVATE src include)
add_executable(typecensus_test test/typecensus_test.cpp)
target_link_libraries(typecensus_test PRIVATE bf6_core_static)
target_include_directories(typecensus_test PRIVATE src include)
add_executable(thermal_test test/thermal_test.cpp)
target_link_libraries(thermal_test PRIVATE bf6_core_static)
target_include_directories(thermal_test PRIVATE src include)
add_executable(restype_test test/restype_test.cpp)
target_link_libraries(restype_test PRIVATE bf6_core_static)
target_include_directories(restype_test PRIVATE src include)
add_executable(resprov_test test/resprov_test.cpp)
target_link_libraries(resprov_test PRIVATE bf6_core_static)
target_include_directories(resprov_test PRIVATE src include)
add_executable(pmvolume_test test/pmvolume_test.cpp)
target_link_libraries(pmvolume_test PRIVATE bf6_core_static)
target_include_directories(pmvolume_test PRIVATE src include)
add_executable(lightprobe_test test/lightprobe_test.cpp)
target_link_libraries(lightprobe_test PRIVATE bf6_core_static)
target_include_directories(lightprobe_test PRIVATE src include)
add_executable(camcensus_test test/camcensus_test.cpp)
target_link_libraries(camcensus_test PRIVATE bf6_core_static)
target_include_directories(camcensus_test PRIVATE src include)

# Direct-install boundary probe for authored UI sound styles/events and their
# installed Harmony sound assets. It makes no claim about the unresolved
# DiceUISoundEventManager selection step.
add_executable(ui_sound_route_probe
  test/ui_sound_route_probe.cpp
  ui_runtime/boot_flow.cpp
  ui_runtime/installed_sound_wave.cpp
  ui_runtime/movie_texture.cpp)
target_link_libraries(ui_sound_route_probe PRIVATE bf6_core_static)
target_include_directories(ui_sound_route_probe PRIVATE src include ui_runtime)
if(WIN32)
  target_link_libraries(ui_sound_route_probe PRIVATE
    mfplat mfreadwrite mfuuid shlwapi ole32 xaudio2)
endif()
if(BUILD_TESTING)
  add_test(NAME game.ui_sound_route COMMAND
    "$<TARGET_FILE:ui_sound_route_probe>" "${BF6_TEST_GAME_DIR}")
  set_tests_properties(game.ui_sound_route PROPERTIES
    LABELS "game;ui;sound;control"
    WORKING_DIRECTORY "${BF6_TEST_OUTPUT_DIR}"
    TIMEOUT 300)
  if(NOT EXISTS "${BF6_TEST_GAME_DIR}")
    set_tests_properties(game.ui_sound_route PROPERTIES DISABLED TRUE)
  endif()
endif()

# The symbolic localisation key: Rime label StringId -> English table, live.
add_executable(ui_string_id_hash_live_test test/ui_string_id_hash_live_test.cpp)
target_link_libraries(ui_string_id_hash_live_test PRIVATE bf6_core_static)
target_include_directories(ui_string_id_hash_live_test PRIVATE src include)

add_executable(frontend_anchor_test test/frontend_anchor_test.cpp)
target_link_libraries(frontend_anchor_test PRIVATE bf6_core_static)
target_include_directories(frontend_anchor_test PRIVATE src include)

add_executable(frontend_named_link_census test/frontend_named_link_census.cpp)
target_link_libraries(frontend_named_link_census PRIVATE bf6_core_static)
target_include_directories(frontend_named_link_census PRIVATE src include)

add_executable(infantry_loadout_default_test
    test/infantry_loadout_default_test.cpp)
target_link_libraries(infantry_loadout_default_test PRIVATE bf6_core_static)
target_include_directories(infantry_loadout_default_test PRIVATE src include)
if(BUILD_TESTING)
  add_test(NAME game.ui_string_id_hash COMMAND
    "$<TARGET_FILE:ui_string_id_hash_live_test>" "${BF6_TEST_GAME_DIR}")
  set_tests_properties(game.ui_string_id_hash PROPERTIES
    LABELS "game;ui;localization;control"
    WORKING_DIRECTORY "${BF6_TEST_OUTPUT_DIR}"
    TIMEOUT 300)
  if(NOT EXISTS "${BF6_TEST_GAME_DIR}")
    set_tests_properties(game.ui_string_id_hash PROPERTIES DISABLED TRUE)
  endif()
endif()

# Installed settings-provider model: Display category -> ordered Graphics rows
# -> localized gameplay-option defaults, with missing-root controls.
add_executable(ui_options_provider_test test/ui_options_provider_test.cpp)
target_link_libraries(ui_options_provider_test PRIVATE bf6_core_static)
target_include_directories(ui_options_provider_test PRIVATE src include)
if(BUILD_TESTING)
  add_test(NAME game.ui_options_provider COMMAND
    "$<TARGET_FILE:ui_options_provider_test>" "${BF6_TEST_GAME_DIR}")
  set_tests_properties(game.ui_options_provider PROPERTIES
    LABELS "game;ui;options;provider;control"
    WORKING_DIRECTORY "${BF6_TEST_OUTPUT_DIR}"
    TIMEOUT 300)
  if(NOT EXISTS "${BF6_TEST_GAME_DIR}")
    set_tests_properties(game.ui_options_provider PROPERTIES DISABLED TRUE)
  endif()
endif()

# The installed enum control selects its visible label with an
# ArrayElementEntityData node fed by the DBD's EnumStrings and Value fields.
# Keep that typed collection route and its fabricated-pin control executable.
add_executable(rime_array_element_live_test
  test/rime_array_element_live_test.cpp
  viewer/rime.cpp
  viewer/rime_list_provider.cpp)
target_link_libraries(rime_array_element_live_test PRIVATE bf6_core_static)
target_include_directories(rime_array_element_live_test PRIVATE
  src include viewer)
if(BUILD_TESTING)
  add_test(NAME game.rime_array_element COMMAND
    "$<TARGET_FILE:rime_array_element_live_test>" "${BF6_TEST_GAME_DIR}")
  set_tests_properties(game.rime_array_element PROPERTIES
    LABELS "game;ui;rime;options;provider;control"
    WORKING_DIRECTORY "${BF6_TEST_OUTPUT_DIR}"
    TIMEOUT 300)
  if(NOT EXISTS "${BF6_TEST_GAME_DIR}")
    set_tests_properties(game.rime_array_element PROPERTIES DISABLED TRUE)
  endif()
endif()

# Label font-style resolution census, live (diagnostic; no CTest entry).
add_executable(ui_label_font_probe test/ui_label_font_probe.cpp)
target_link_libraries(ui_label_font_probe PRIVATE bf6_core_static)
target_include_directories(ui_label_font_probe PRIVATE src include)

# Research: what the glass tint palette constant 0xA0106346 actually holds, so
# the authored dual-source composite can use the real tint instead of a guess.
add_executable(glass_tint_probe test/glass_tint_probe.cpp)
target_link_libraries(glass_tint_probe PRIVATE bf6_core_static)
target_include_directories(glass_tint_probe PRIVATE src include)

# The authored glass transmission palette reaches a consumer through the ABI.
add_executable(glass_tint_live_test test/glass_tint_live_test.cpp)
target_link_libraries(glass_tint_live_test PRIVATE bf6_core_static)
target_include_directories(glass_tint_live_test PRIVATE src include)
if(EXISTS "${BF6_TEST_GAME_DIR}")
  add_test(NAME game.glass_tint COMMAND
    "$<TARGET_FILE:glass_tint_live_test>" "${BF6_TEST_GAME_DIR}")
endif()

# Research: every field on every UIGadgetAbilityMetadata row, so the gadget
# join can stop being a name-similarity score.
add_executable(gadget_metadata_fields_probe test/gadget_metadata_fields_probe.cpp)
target_link_libraries(gadget_metadata_fields_probe PRIVATE bf6_core_static)
target_include_directories(gadget_metadata_fields_probe PRIVATE src include)

# Research: independent re-measurement of the gadget metadata table and the
# infantry loadout templates, walked from the partitions themselves rather than
# through the library helpers that already assert the answer.
add_executable(gadget_loadout_audit_probe test/gadget_loadout_audit_probe.cpp)
target_link_libraries(gadget_loadout_audit_probe PRIVATE bf6_core_static)
target_include_directories(gadget_loadout_audit_probe PRIVATE src include)

# Research: which constraint in the RigAsset span join actually fails.
add_executable(anim_rig_span_probe test/anim_rig_span_probe.cpp)
target_link_libraries(anim_rig_span_probe PRIVATE bf6_core_static)
target_include_directories(anim_rig_span_probe PRIVATE src include)

# Research: what the DBD TypeRef signature means, by census rather than guess.
add_executable(dbd_type_census test/dbd_type_census.cpp)
target_link_libraries(dbd_type_census PRIVATE bf6_core_static)
target_include_directories(dbd_type_census PRIVATE src include)

# Does a level have a water surface block (streaming-tree block 2) at all?
add_executable(waterblock_probe test/waterblock_probe.cpp)
target_link_libraries(waterblock_probe PRIVATE bf6_core_static)
target_include_directories(waterblock_probe PRIVATE src include)

# Where are the 869 operator keys registered that neither known route explains?
add_executable(expr_dark_probe test/expr_dark_probe.cpp)
target_link_libraries(expr_dark_probe PRIVATE bf6_core_static)
target_include_directories(expr_dark_probe PRIVATE src include)

# The pose-trajectory matching database, field by field.
add_executable(ptm_probe test/ptm_probe.cpp)
target_link_libraries(ptm_probe PRIVATE bf6_core_static)
target_include_directories(ptm_probe PRIVATE src include)
