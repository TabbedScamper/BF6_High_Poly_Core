# Repeatable libbf6 validation suites.
#
# Every game-facing test reads the user's installed Steam copy at execution
# time. Research TSV/JSON files are not passed as runtime inputs. Configure a
# non-default Steam library with -DBF6_TEST_GAME_DIR=... .

set(BF6_TEST_GAME_DIR "$ENV{BF6_GAME_DIR}" CACHE PATH
    "Battlefield 6 Steam installation used by integration tests")
if(NOT BF6_TEST_GAME_DIR AND WIN32)
  set(_bf6_standard_steam "C:/Program Files (x86)/Steam/steamapps/common/Battlefield 6")
  if(EXISTS "${_bf6_standard_steam}/SP/bf6.exe")
    set(BF6_TEST_GAME_DIR "${_bf6_standard_steam}" CACHE PATH "" FORCE)
  endif()
endif()

set(BF6_TEST_EXE "${BF6_TEST_GAME_DIR}/SP/bf6.exe" CACHE FILEPATH
    "Unencrypted Steam executable used for reflected type reads")
set(BF6_TEST_LEVEL "mp_portal_sand" CACHE STRING
    "Level used by the default map integration suite")
get_filename_component(BF6_PORTAL_SDK_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(BF6_TEST_PORTAL_EXPORT "${BF6_PORTAL_SDK_ROOT}/FbExportData" CACHE PATH
    "Portal SDK FbExportData used only by the catalogue verification test")
set(BF6_TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}/test-output" CACHE PATH
    "Scratch output for native validation harnesses")
file(MAKE_DIRECTORY "${BF6_TEST_OUTPUT_DIR}")

function(bf6_register name target labels)
  if(NOT TARGET "${target}")
    return()
  endif()
  add_test(NAME "${name}" COMMAND "$<TARGET_FILE:${target}>" ${ARGN})
  set_tests_properties("${name}" PROPERTIES
    LABELS "${labels}"
    WORKING_DIRECTORY "${BF6_TEST_OUTPUT_DIR}"
    TIMEOUT 300)
endfunction()

bf6_register(unit.rime_text_runtime rime_text_runtime_test
  "unit;ui;rime;text;markup")
bf6_register(unit.viewer_packed_color viewer_packed_color_test
  "unit;ui;viewer;color;control")
bf6_register(game.rime_markup_svg rime_markup_svg_live_test
  "game;ui;rime;text;markup;control" "${BF6_TEST_GAME_DIR}")
bf6_register(game.loadout_class_icons loadout_class_icon_live_test
  "game;ui;loadout;texture;control" "${BF6_TEST_GAME_DIR}")

# SDK-only oracle. It is deliberately separate from runtime suites because the
# engine bindings must never consume the SDK's exported catalogue.
bf6_register(sdk.placeables placeables_test "sdk;oracle"
  "${BF6_TEST_PORTAL_EXPORT}")
if(NOT EXISTS "${BF6_TEST_PORTAL_EXPORT}")
  set_tests_properties(sdk.placeables PROPERTIES DISABLED TRUE)
endif()

# Smallest useful installed-game smoke suite: archive discovery and the broad
# typed decoders. Each harness contains its own fake/shuffled control where the
# underlying finding requires one.
bf6_register(game.level_catalog level_test "game;smoke" "${BF6_TEST_GAME_DIR}" levels)
bf6_register(game.water water_test "game;water;smoke" "${BF6_TEST_GAME_DIR}" "${BF6_TEST_LEVEL}")
bf6_register(game.water_closure waterclosure_test "game;water;control"
  "${BF6_TEST_GAME_DIR}" "mp_isolated")
bf6_register(game.water_heightfield waterheightfield_test "game;water;terrain;control"
  "${BF6_TEST_GAME_DIR}" "mp_isolated")
bf6_register(game.water_depth waterdepth_test "game;water;terrain" "${BF6_TEST_GAME_DIR}" "${BF6_TEST_LEVEL}")
bf6_register(game.terrain terrain_test "game;terrain;smoke" "${BF6_TEST_GAME_DIR}" "${BF6_TEST_LEVEL}")
bf6_register(game.terrain_bounds_live terrainbounds_live_test "game;terrain;control"
  "${BF6_TEST_GAME_DIR}" "${BF6_TEST_LEVEL}")
bf6_register(game.terrain_layers terrainlayers_test "game;terrain;control" "${BF6_TEST_GAME_DIR}" "${BF6_TEST_LEVEL}")
bf6_register(game.terrain_static terrainstatic_test "game;terrain;control" "${BF6_TEST_GAME_DIR}" "${BF6_TEST_LEVEL}")
bf6_register(game.terrain_static_live terrainstatic_live_test "game;terrain;dxil;control"
  "${BF6_TEST_GAME_DIR}" "mp_isolated"
  "${BF6_PORTAL_SDK_ROOT}/BF6_Frostbite_Research/data/terrain_static_layer_map.tsv")
bf6_register(game.decals decals_test "game;decals;smoke"
  "${BF6_TEST_GAME_DIR}" "mp_isolated")
bf6_register(game.scatter scatter_test "game;scatter;control" "${BF6_TEST_GAME_DIR}" "${BF6_TEST_LEVEL}")
bf6_register(game.lighting_zones lightingzones_test "game;lighting;control"
  "${BF6_TEST_GAME_DIR}" "mp_dumbo")
bf6_register(game.fx_abi fxabi_test "game;fx;abi;control"
  "${BF6_TEST_GAME_DIR}" "mp_atoll")
bf6_register(game.weapon_fit weapon_fit_test "game;armory;control" "${BF6_TEST_GAME_DIR}")
bf6_register(game.rime_live rime_live_test "game;ui;rime;control"
  "${BF6_TEST_GAME_DIR}"
  "${BF6_PORTAL_SDK_ROOT}/BF6_Frostbite_Research/data/rime_render_data.tsv")
bf6_register(game.rime_dynamic_list_cells rime_dynamic_list_cell_test
  "game;ui;rime;control" "${BF6_TEST_GAME_DIR}")
bf6_register(game.rime_runtime rime_runtime_install_test
  "game;ui;rime;runtime;control" "${BF6_TEST_GAME_DIR}")
bf6_register(game.home_linear_animation home_linear_animation_runtime_test
  "game;ui;rime;animation;control" "${BF6_TEST_GAME_DIR}")
bf6_register(game.direct_screen_animation_apply direct_screen_animation_apply_test
  "game;ui;rime;animation;integration;control" "${BF6_TEST_GAME_DIR}")
bf6_register(game.home_composed_animation_route home_composed_animation_route_test
  "game;ui;rime;animation;integration;control" "${BF6_TEST_GAME_DIR}")
bf6_register(game.ui_compositor_gate ui_compositor_gate
  "game;ui;rime;compositor;control" "${BF6_TEST_GAME_DIR}"
  --allow-partial)
if(NOT EXISTS "${BF6_TEST_GAME_DIR}" OR NOT EXISTS "${BF6_TEST_EXE}")
  get_property(_bf6_tests DIRECTORY PROPERTY TESTS)
  foreach(_test IN LISTS _bf6_tests)
    if(_test MATCHES "^game\\.")
      set_tests_properties("${_test}" PROPERTIES DISABLED TRUE)
    endif()
  endforeach()
endif()
