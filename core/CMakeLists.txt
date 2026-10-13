set(RECAST_DIR ${recastnavigation_SOURCE_DIR}/Recast)
add_library(recast STATIC
    ${RECAST_DIR}/Source/Recast.cpp
    ${RECAST_DIR}/Source/RecastAlloc.cpp
    ${RECAST_DIR}/Source/RecastArea.cpp
    ${RECAST_DIR}/Source/RecastAssert.cpp
    ${RECAST_DIR}/Source/RecastContour.cpp
    ${RECAST_DIR}/Source/RecastFilter.cpp
    ${RECAST_DIR}/Source/RecastLayers.cpp
    ${RECAST_DIR}/Source/RecastMesh.cpp
    ${RECAST_DIR}/Source/RecastMeshDetail.cpp
    ${RECAST_DIR}/Source/RecastRasterization.cpp
    ${RECAST_DIR}/Source/RecastRegion.cpp)
target_include_directories(recast SYSTEM PUBLIC ${RECAST_DIR}/Include)
# Same float rules as splashcore: the nav mesh has to match DotRecast's.
if(MSVC)
    target_compile_options(recast PRIVATE /fp:precise)
else()
    target_compile_options(recast PRIVATE -ffp-contract=off)
endif()

# psxlua's compiler with PS1 number semantics (core/psxlua/psxlua_host.h,
# force-included) and PS1 chunk layout (core/psxlua/ldump_psx.c).
set(PSXLUA_DIR ${psxlua_SOURCE_DIR}/src)
add_library(psxlua STATIC
    ${PSXLUA_DIR}/lapi.c
    ${PSXLUA_DIR}/lcode.c
    ${PSXLUA_DIR}/lctype.c
    ${PSXLUA_DIR}/ldebug.c
    ${PSXLUA_DIR}/ldo.c
    ${PSXLUA_DIR}/lfunc.c
    ${PSXLUA_DIR}/lgc.c
    ${PSXLUA_DIR}/llex.c
    ${PSXLUA_DIR}/llibc.c
    ${PSXLUA_DIR}/lmem.c
    ${PSXLUA_DIR}/lobject.c
    ${PSXLUA_DIR}/lopcodes.c
    ${PSXLUA_DIR}/lparser.c
    ${PSXLUA_DIR}/lstate.c
    ${PSXLUA_DIR}/lstring.c
    ${PSXLUA_DIR}/ltable.c
    ${PSXLUA_DIR}/ltm.c
    ${PSXLUA_DIR}/lundump.c
    ${PSXLUA_DIR}/lvm.c
    ${PSXLUA_DIR}/lzio.c
    psxlua/ldump_psx.c
    psxlua/psxlua_compile.c)
target_include_directories(psxlua PRIVATE ${PSXLUA_DIR})
target_include_directories(psxlua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/psxlua)
target_compile_definitions(psxlua PRIVATE LUA_COMPAT_ALL)
set(PSXLUA_HOST_H ${CMAKE_CURRENT_SOURCE_DIR}/psxlua/psxlua_host.h)
if(MSVC)
    target_compile_options(psxlua PRIVATE /FI${PSXLUA_HOST_H} /w)
else()
    target_compile_options(psxlua PRIVATE -include ${PSXLUA_HOST_H} -w -fsigned-char -fwrapv)
endif()

# libpsxav (the encoder psxavenc uses), plain C.
add_library(psxav STATIC ${psxavenc_SOURCE_DIR}/libpsxav/adpcm.c ${psxavenc_SOURCE_DIR}/libpsxav/cdrom.c)
target_include_directories(psxav PUBLIC ${psxavenc_SOURCE_DIR}/libpsxav)
# C11 for _Static_assert (MSVC defaults to an older C).
set_target_properties(psxav PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(psxav PRIVATE -w)
endif()

add_library(splashcore STATIC
    audio.cpp
    blend.cpp
    disc.cpp
    font.cpp
    scene.cpp
    skin.cpp
    texture.cpp
    vrampacker.cpp
    bvh.cpp
    navregion.cpp
    splashpack.cpp
    worldstream.cpp
    luacompile.cpp
    import.cpp
    ${ufbx_SOURCE_DIR}/ufbx.c
    stb_impl.cpp)
target_include_directories(splashcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(splashcore SYSTEM PRIVATE ${stb_SOURCE_DIR} ${dr_libs_SOURCE_DIR} ${cgltf_SOURCE_DIR} ${ufbx_SOURCE_DIR})
target_link_libraries(splashcore PUBLIC nlohmann_json::nlohmann_json PRIVATE recast psxlua psxav)
# The writer reproduces SplashEdit 2.4's float results bit for bit: no FMA
# contraction, no fast-math.
if(MSVC)
    target_compile_options(splashcore PRIVATE /W4 /fp:precise)
else()
    target_compile_options(splashcore PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
