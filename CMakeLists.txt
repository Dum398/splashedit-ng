cmake_minimum_required(VERSION 3.24)
project(splashedit-ng LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b-docking
    GIT_SHALLOW TRUE)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)
# Recast only (not Detour, DebugUtils or the demo). SOURCE_SUBDIR names a
# directory without a CMakeLists.txt so this just fetches; core/CMakeLists.txt
# builds the sources. Pinned to the upstream commit (#796) that the DotRecast
# build used by SplashEdit 2.4.0 was last synced to.
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG 9f4ce64458dfae86e1239c525ddc219c4e9e06f1
    SOURCE_SUBDIR Recast/Source)
# psxlua, the Lua 5.2 fork the engine runs, for the host-side compiler.
# Fetch only (src has no CMakeLists.txt); core/CMakeLists.txt builds it.
# Pin to the commit psxsplash's nugget uses.
FetchContent_Declare(psxlua
    GIT_REPOSITORY https://github.com/rixnobis/psxlua.git
    GIT_TAG 4401a3354e1ffe5f38b028c4d5cedf7c077638bd
    SOURCE_SUBDIR src)
# libpsxav's SPU-ADPCM encoder (the one psxavenc uses), and dr_wav to read WAV.
FetchContent_Declare(psxavenc
    GIT_REPOSITORY https://codeberg.org/WonderfulToolchain/psxavenc.git
    GIT_TAG f6757f823b707114c22997b7841a8a2898e800b6
    SOURCE_SUBDIR none)
FetchContent_Declare(dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG dfe8377631000664666519fdb83da193fd8037f4
    SOURCE_SUBDIR none)
# cgltf reads glTF 2.0 for model import.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG 360db1a95480fe102ae9c69b27c5d101167ff5ba
    SOURCE_SUBDIR none)
# ufbx reads FBX and OBJ (with MTL) for model import.
FetchContent_Declare(ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG 26a482ae66871d7de36eb722aa060bce95bce274
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(stb nlohmann_json recastnavigation psxlua psxavenc dr_libs cgltf ufbx)

option(SPLASHEDIT_BUILD_EDITOR "Build the editor (needs SDL3, FreeType, OpenGL)" ON)

add_subdirectory(core)
add_subdirectory(cli)

# Regenerates examples/courtyard (meshes, crate texture, scene). Not built by
# default: `cmake --build build --target make_example && ./build/make_example`.
add_executable(make_example EXCLUDE_FROM_ALL tools/make_example.cpp)
target_link_libraries(make_example PRIVATE splashcore)
target_compile_definitions(make_example PRIVATE SPLASHEDIT_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/courtyard")
if(MSVC)
    target_compile_options(make_example PRIVATE /W4)
else()
    target_compile_options(make_example PRIVATE -Wall -Wextra)
endif()

# The editor's document model (history, picking): no SDL or GL, so it builds
# and tests headless, also with SPLASHEDIT_BUILD_EDITOR off.
add_library(splasheditdoc STATIC
    src/editor/asset_browser.cpp
    src/editor/catalog.cpp
    src/editor/memcard.cpp
    src/editor/document.cpp
    src/editor/pick.cpp
    src/editor/gizmo.cpp
    src/editor/live_export.cpp
    src/editor/lua_editor.cpp
    src/editor/play.cpp
    src/editor/model_import.cpp
    src/editor/project.cpp
    src/editor/serial.cpp
    src/editor/timeline.cpp
    src/editor/hardware.cpp
    src/editor/psxmon.cpp
    src/editor/texture_preview.cpp
    src/editor/unirom.cpp)
target_include_directories(splasheditdoc PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(splasheditdoc PUBLIC splashcore Threads::Threads)
if(WIN32)
    target_link_libraries(splasheditdoc PUBLIC ws2_32)
endif()
if(MSVC)
    target_compile_options(splasheditdoc PRIVATE /W4 /utf-8)
else()
    target_compile_options(splasheditdoc PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(editor_tests src/editor/tests/editor_tests.cpp)
target_link_libraries(editor_tests PRIVATE splasheditdoc)
target_compile_definitions(editor_tests PRIVATE SPLASHEDIT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(editor_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(editor_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME editor_tests COMMAND editor_tests)

if(NOT SPLASHEDIT_BUILD_EDITOR)
    return()
endif()

FetchContent_MakeAvailable(imgui)

# Use system SDL3 and FreeType when present, otherwise build them, so a
# plain Windows checkout configures without a package manager.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.16
        GIT_SHALLOW TRUE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
endif()
find_package(Freetype QUIET)
if(NOT FREETYPE_FOUND)
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-14-3
        GIT_SHALLOW TRUE)
    foreach(dep BZIP2 PNG ZLIB BROTLI HARFBUZZ)
        set(FT_DISABLE_${dep} ON CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_MakeAvailable(freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC SDL3::SDL3 Freetype::Freetype OpenGL::GL)

# Emulator::stop against a process that will not close when asked.
add_executable(emulator_tests src/emulator_tests.cpp src/emulator.cpp)
target_include_directories(emulator_tests PRIVATE src)
target_link_libraries(emulator_tests PRIVATE SDL3::SDL3)
add_test(NAME emulator_tests COMMAND emulator_tests)

add_executable(splashedit
    src/main.cpp
    src/emulator.cpp
    src/game_view.cpp
    src/gl.cpp
    src/ui/brand.cpp
    src/ui/theme.cpp
    src/ui/splash.cpp
    src/ui/widgets.cpp
    src/viewport/ps1view.cpp
    src/mockup/main_screen.cpp
    src/editor/lua_editor_view.cpp
    src/editor/timeline_view.cpp
    src/editor/asset_browser_view.cpp)
target_include_directories(splashedit PRIVATE src)
target_include_directories(splashedit SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(splashedit PRIVATE imgui splasheditdoc splashcore)
target_compile_definitions(splashedit PRIVATE SPLASHEDIT_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    SPLASHEDIT_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples/courtyard")
if(WIN32)
    target_compile_definitions(splashedit PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(splashedit PRIVATE /W4 /utf-8)
else()
    target_compile_options(splashedit PRIVATE -Wall -Wextra)
endif()
