cmake_minimum_required(VERSION 3.24)
# third-party submodules still declare <3.5 minimums; CMake 4 refuses those without this
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# 2026-10-29: deps/ holds the same two submodules the sibling mods use (cyberpunk_cmake, red4ext.sdk). Until they
# are checked out here, DEBUG_DRAW_DEPS_DIR can point at another checkout's deps/ (read only, nothing is written
# there), e.g. -DDEBUG_DRAW_DEPS_DIR=E:/src/let_there_be_flight/deps
set(DEBUG_DRAW_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps" CACHE PATH "Folder holding cyberpunk_cmake and red4ext.sdk")

list(APPEND CMAKE_MODULE_PATH "${DEBUG_DRAW_DEPS_DIR}/cyberpunk_cmake")
list(APPEND CMAKE_MODULE_PATH "${DEBUG_DRAW_DEPS_DIR}/red4ext.sdk/cmake")

include(ConfigureVersionFromGit)
configure_version_from_git()

project(debug_draw VERSION ${GIT_VERSION_MAJOR}.${GIT_VERSION_MINOR}.${GIT_VERSION_PATCH} LANGUAGES NONE)

include(CyberpunkMod)

configure_mod(
  NAME "Debug Draw"
  SLUG ${PROJECT_NAME}
  AUTHOR "Jack Humbert"
  URL "https://github.com/jackhumbert/cp2077_debug_draw"
  LICENSE "Licensed under the MIT license. See the license.md in the root project for details."
)

set(LOAD_REDSCRIPT_FROM_RED4EXT ON)

# 2026-10-29: FindRED4ext (pulled in by configure_red4ext) downloads the latest RED4ext release into
# game_dir_requirements on every fresh configure. This mod ships no requirements folder, so skip it.
set(MOD_RED4EXT_DEPENDENCY_ADDED ON)
list(APPEND MOD_REQUIREMENTS "RED4ext 1.31.0+")

# 2026-10-29: cyberpunk_cmake's FindRED4ext.SDK hardcodes ${MOD_SOURCE_DIR}/deps/red4ext.sdk. Adding the target
# first makes it skip, so DEBUG_DRAW_DEPS_DIR also works for the SDK.
if(NOT TARGET RED4ext.SDK)
  option(RED4EXT_USE_PCH "" ON)
  option(RED4EXT_HEADER_ONLY "" ON)
  add_subdirectory("${DEBUG_DRAW_DEPS_DIR}/red4ext.sdk" "${CMAKE_CURRENT_BINARY_DIR}/deps/red4ext.sdk")
  set_target_properties(RED4ext.SDK PROPERTIES FOLDER "Dependencies")
  set(MOD_RED4EXT_SDK_DIR "${DEBUG_DRAW_DEPS_DIR}/red4ext.sdk")
endif()

configure_red4ext(src/red4ext)
target_compile_features(${MOD_SLUG}.dll PRIVATE cxx_std_20)

configure_redscript(src/redscript)

# Built-in shader effects, as tools/shaderc writes them (archive/pc/mod/... and red4ext/plugins/debug_draw/shaders/),
# kept in the repo so a build needs neither dxc nor WolvenKit. Copied at configure time too: configure_install globs
# game_dir's top-level folders then, and archive/ only exists once this has run.
set(DEBUG_DRAW_GAME_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/game_files")
if(EXISTS "${DEBUG_DRAW_GAME_FILES}")
  file(COPY "${DEBUG_DRAW_GAME_FILES}/" DESTINATION "${MOD_GAME_DIR}")
  add_custom_target(${MOD_SLUG}_game_files ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${DEBUG_DRAW_GAME_FILES}" "${MOD_GAME_DIR}"
    COMMENT "Copying the built-in shader effects into game_dir")
endif()

# Unit tests for the parts of the plugin that need no game (tests/): ctest --test-dir build, or run
# build/debug_draw_tests.exe. The sources they cover are compiled in again here, so the test exe needs no game.
option(DEBUG_DRAW_TESTS "Build the unit tests" ON)
if(DEBUG_DRAW_TESTS)
  enable_testing()
  add_executable(debug_draw_tests
    tests/main.cpp
    tests/geometry_tests.cpp
    tests/items_tests.cpp
    tests/shader_manifest_tests.cpp
    src/red4ext/Items.cpp
    src/red4ext/ShaderManifest.cpp)
  target_include_directories(debug_draw_tests PRIVATE src/red4ext)
  target_compile_features(debug_draw_tests PRIVATE cxx_std_20)
  target_compile_definitions(debug_draw_tests PRIVATE DEBUG_DRAW_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(debug_draw_tests PRIVATE RED4ext.SDK)
  add_test(NAME debug_draw_tests COMMAND debug_draw_tests)
endif()

# The overlay toggle key's mapping (src/input_loader/debug_draw.xml) is not shipped while the key is off; see
# Settings.reds.

configure_folder_file(readme.md)
configure_folder_file(license.md)
configure_folder_file(third_party_notices.md)
configure_release(${MOD_SLUG}_${MOD_VERSION_STR}.zip)
configure_install()
