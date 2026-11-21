# PS Vita build: clang compiles, VitaSDK's GCC links and its tools make the VPK.
#
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/vita/toolchain.cmake
#   ninja -C build
#
# Needs VITASDK (with the vitaGL, vitaShaRK, libmathneon, SceShaccCgExt packages: vdpm install ...)
# and clang 17 or newer with the ARM target.
if (NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Set VITASDK to the VitaSDK directory")
endif()
set(VITASDK $ENV{VITASDK})

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv7-a)
set(VITA_CLANG TRUE)
set(VITA TRUE)

find_program(CLANG NAMES clang clang-20 clang-19 clang-18 clang-17 REQUIRED)
set(CMAKE_C_COMPILER ${CLANG})
set(CMAKE_ASM_COMPILER ${CLANG})
set(CMAKE_C_COMPILER_TARGET armv7a-none-eabihf)
set(CMAKE_ASM_COMPILER_TARGET armv7a-none-eabihf)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The Vita's CPU (Cortex-A9, NEON, hard float, Thumb-2) and what VitaSDK's GCC defines for it.
set(VITA_ARCH_FLAGS "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -D__vita__ -fno-pic -fno-pie")
set(VITA_GCC_INCLUDE "${VITASDK}/lib/gcc/arm-vita-eabi")
file(GLOB VITA_GCC_VERSION_DIR LIST_DIRECTORIES true "${VITA_GCC_INCLUDE}/*")
list(GET VITA_GCC_VERSION_DIR 0 VITA_GCC_VERSION_DIR)
set(CMAKE_C_FLAGS_INIT "${VITA_ARCH_FLAGS} -isystem ${VITASDK}/arm-vita-eabi/include")
set(CMAKE_ASM_FLAGS_INIT "${VITA_ARCH_FLAGS}")

# Linking with VitaSDK's GCC driver: its newlib, its startup files, the system's stub libraries.
set(VITA_LINKER ${VITASDK}/bin/arm-vita-eabi-gcc)
set(CMAKE_C_LINK_EXECUTABLE
    "${VITA_LINKER} -Wl,-q <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

# The SDK's tools, as its own toolchain file (vita.toolchain.cmake) names them for vita.cmake's macros.
foreach(TOOL ELF_CREATE ELF_EXPORT LIBS_GEN MAKE_FSELF MKSFOEX PACK_VPK)
    string(TOLOWER ${TOOL} NAME)
    string(REPLACE "_" "-" NAME ${NAME})
    set(VITA_${TOOL} "${VITASDK}/bin/vita-${NAME}" CACHE PATH "vita-${NAME}")
endforeach()

set(CMAKE_FIND_ROOT_PATH ${VITASDK}/arm-vita-eabi)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
