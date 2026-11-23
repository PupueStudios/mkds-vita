cmake_minimum_required(VERSION 3.22)
project(mkds_vita C)

# PS Vita build:
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/vita/toolchain.cmake
#   ninja -C build
if (NOT VITA_CLANG)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=<repo>/vita/toolchain.cmake")
endif()

set(R ${CMAKE_CURRENT_SOURCE_DIR})
set(P ${R}/platform)

# ------------------------------------------------------------------ the game
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS ${R}/game/*.c)
add_library(game STATIC ${GAME_SOURCES})
target_include_directories(game PRIVATE ${R}/include ${R}/msl_include ${P}/include)
target_compile_definitions(game PRIVATE version_EUR)
target_compile_options(game PRIVATE
    -std=gnu11
    -include ${R}/include/port_prefix.h
    -O2
    -fno-strict-aliasing
    -fwrapv
    -fcommon
    -fshort-wchar
    -Dconst=
    -fno-zero-initialized-in-bss
    -ffunction-sections
    -w)
foreach(SRC ${GAME_SOURCES})
    if (SRC MATCHES "/game/libs/")
        set_source_files_properties(${SRC} PROPERTIES COMPILE_OPTIONS -fsigned-char)
    else()
        set_source_files_properties(${SRC} PROPERTIES COMPILE_OPTIONS -funsigned-char)
    endif()
endforeach()

# ------------------------------------------------------------------ vitaGL, built with MSAA render targets
set(VGL_SRC ${R}/vita/vitagl)
set(VGL_DIR ${CMAKE_BINARY_DIR}/vitagl)
file(GLOB_RECURSE VGL_FILES CONFIGURE_DEPENDS ${VGL_SRC}/source/*)
add_custom_command(OUTPUT ${VGL_DIR}/libvitaGL.a
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${VGL_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${VGL_SRC} ${VGL_DIR}
    COMMAND ${CMAKE_COMMAND} -E env PATH=${VITASDK}/bin:$ENV{PATH}
        make -C ${VGL_DIR} -j8 NO_DEBUG=1 HAVE_ACCURATE_MSAA=1
    DEPENDS ${VGL_FILES} ${VGL_SRC}/Makefile
    COMMENT "Building vitaGL")
add_custom_target(vitagl_build DEPENDS ${VGL_DIR}/libvitaGL.a)
add_library(vitagl_msaa STATIC IMPORTED)
set_target_properties(vitagl_msaa PROPERTIES IMPORTED_LOCATION ${VGL_DIR}/libvitaGL.a)
add_dependencies(vitagl_msaa vitagl_build)

# ------------------------------------------------------------------ the DS platform layer and the Vita executable
file(GLOB PLATFORM_CORE CONFIGURE_DEPENDS ${P}/*.c)
file(GLOB VITA_SOURCES CONFIGURE_DEPENDS ${P}/vita/*.c)
file(GLOB GL_SOURCES CONFIGURE_DEPENDS ${P}/gl/*.c)
set(XZ_SOURCES ${R}/vita/xz/xz_crc32.c ${R}/vita/xz/xz_crc64.c ${R}/vita/xz/xz_dec_stream.c ${R}/vita/xz/xz_dec_lzma2.c)
set_source_files_properties(${XZ_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-DXZ_USE_CRC64")
add_executable(mkds ${PLATFORM_CORE} ${P}/hostside/direct.c ${VITA_SOURCES} ${GL_SOURCES} ${XZ_SOURCES})
target_include_directories(mkds BEFORE PRIVATE ${VGL_SRC}/source)
target_include_directories(mkds PRIVATE ${R}/include ${P}/include ${P} ${P}/gl)
target_compile_definitions(mkds PRIVATE version_EUR PORT_PLATFORM_LAYER PORT_GUEST_STACK=524288)
target_compile_options(mkds PRIVATE -O2 -fno-strict-aliasing -fwrapv -Wall -Wno-unused-function -std=gnu11)
set_source_files_properties(${VITA_SOURCES} ${GL_SOURCES} PROPERTIES COMPILE_OPTIONS -fshort-enums)
set_source_files_properties(${P}/gpu2d.c ${P}/tex.c ${P}/soft3d.c ${P}/gx3d.c ${P}/sound.c PROPERTIES
    COMPILE_OPTIONS "-O3;-fomit-frame-pointer")
target_link_options(mkds PRIVATE -Wl,--no-undefined -Wl,--no-wchar-size-warning -Wl,--no-enum-size-warning
    -Wl,--defsym,__sce_headroom=0x2000)
target_link_libraries(mkds PRIVATE
    -Wl,--whole-archive game -Wl,--no-whole-archive
    vita2d_vgl freetype png bz2 z ScePgf_stub ScePvf_stub
    vitagl_msaa vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
    SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub
    SceSysmodule_stub SceCtrl_stub SceTouch_stub SceAudio_stub ScePower_stub SceNet_stub SceNetCtl_stub
    SceRtc_stub SceIofilemgr_stub SceLibKernel_stub SceKernelThreadMgr_stub SceRazorCapture_stub
    stdc++ pthread m c)

# ------------------------------------------------------------------ VPK
include(${VITASDK}/share/vita.cmake)
set(VITA_APP_NAME "Mario Kart DS")
set(VITA_TITLEID "MKDS00001")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
vita_create_self(eboot.bin mkds UNSAFE)
set(LIVEAREA ${R}/vita/sce_sys)
set(VPK_FILES)
foreach(F icon0.png livearea/contents/bg.png livearea/contents/startup.png livearea/contents/template.xml)
    if (EXISTS ${LIVEAREA}/${F})
        list(APPEND VPK_FILES FILE ${LIVEAREA}/${F} sce_sys/${F})
    endif()
endforeach()
vita_create_vpk(mkds.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME} ${VPK_FILES})
