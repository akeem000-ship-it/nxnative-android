# The host as an Android shared library (libnxhost.so, loaded by the app in dm/android/app), built with the NDK for arm64-v8a.
# Configure with the NDK's toolchain file and the game profile, like dm/host/Makefile's GAME:
#   cmake -S dm/android -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DGAME=<title ID>
# The upstream sources build unchanged where they can: the Darwin headers they include come from compat/include (darwin_compat.h is force-included), the Mach-O assembly is
# converted to ELF at build time (tools/asm_elf.py), and the Apple-only pieces (front end, Metal, VideoToolbox, Core Haptics, memstat) are replaced by the files in this directory
cmake_minimum_required(VERSION 3.22)
project(nxhost C ASM)

set(GAME "" CACHE STRING "Base game title ID of the profile (dm/games/<id>)")
if(NOT GAME)
    message(FATAL_ERROR "GAME is not set: -DGAME=<base game title ID> (python3 dm/games/game.py list)")
endif()
get_filename_component(DM ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Game profile header, as the Makefile makes it (a profile outside the repository is found through DM_GAMES_DIR by game.py)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_target(game_profile ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${Python3_EXECUTABLE} ${DM}/games/game.py header ${GAME} ${GEN}/game_profile.h
    BYPRODUCTS ${GEN}/game_profile.h
    COMMENT "game profile ${GAME}")

# libvpx (VP8 / VP9 movies) as in the Makefile, with its arm64 configuration headers
file(GLOB_RECURSE VPX_SRC ${DM}/third_party/libvpx/*.c)
add_library(vpx STATIC ${VPX_SRC})
target_include_directories(vpx PRIVATE ${DM}/third_party/libvpx ${DM}/third_party/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w)

# Mach-O assembly → ELF
set(ASM_IN ${DM}/host/entry.S ${DM}/gfx/nvn/nvn_entry.S ${DM}/gfx/nvn/nvn_thunks.S)
set(ASM_OUT)
foreach(f ${ASM_IN})
    get_filename_component(n ${f} NAME)
    add_custom_command(OUTPUT ${GEN}/asm/${n}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}/asm
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py ${f} ${GEN}/asm/${n}
        DEPENDS ${f} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py)
    list(APPEND ASM_OUT ${GEN}/asm/${n})
endforeach()

# Host and graphics C sources (by wildcard, as the Makefile; the VideoToolbox decoder is replaced by movie_avc_mediacodec.c)
file(GLOB HOST_SRC ${DM}/host/*.c)
list(FILTER HOST_SRC EXCLUDE REGEX "hle_movie_avc\\.c$")
file(GLOB GFX_SRC ${DM}/gfx/*.c ${DM}/gfx/nvn/*.c ${DM}/gfx/tex/*.c)
# The graphics backend: vulkan (gfx/nvn_vk.c, with its format table) or null (gfx/nvn_null.c: nothing drawn)
set(GFX vulkan CACHE STRING "Graphics backend: vulkan or null")
file(GLOB ANDROID_SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/compat/*.c)
if(GFX STREQUAL "null")
    list(APPEND ANDROID_SRC ${CMAKE_CURRENT_SOURCE_DIR}/gfx/nvn_null.c)
elseif(GFX STREQUAL "vulkan")
    list(APPEND ANDROID_SRC ${CMAKE_CURRENT_SOURCE_DIR}/gfx/nvn_vk.c ${CMAKE_CURRENT_SOURCE_DIR}/gfx/nvn_fmt_vk.c)
else()
    message(FATAL_ERROR "GFX must be vulkan or null: ${GFX}")
endif()

add_library(nxhost SHARED ${HOST_SRC} ${GFX_SRC} ${ANDROID_SRC} ${ASM_OUT})
add_dependencies(nxhost game_profile)
target_include_directories(nxhost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/include    # first: the Darwin stand-ins
    ${GEN} ${DM}/host ${DM}/gfx/nvn/include ${DM}/third_party/vulkan-headers/include ${DM}/third_party/libvpx ${DM}/third_party/stb_vorbis
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_definitions(nxhost PRIVATE _GNU_SOURCE)
target_compile_options(nxhost PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/darwin_compat.h>
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    -O1 -g -Wall -Wno-unused-function
    -ffixed-x18)   # as on Apple: guest code never sees x18 (the rewriter moves it to a thread slot), and the host must not use it either (bionic's shadow call stack)
# The NativeActivity glue, built in (android_main is in front_android.c)
add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_compile_options(native_app_glue PRIVATE -w)
target_link_libraries(nxhost PRIVATE native_app_glue vpx android log aaudio mediandk vulkan m dl)
# -Bsymbolic: the assembly reaches host globals with adrp / add, which needs them bound inside this library; ANativeActivity_onCreate must stay exported for the glue
target_link_options(nxhost PRIVATE -Wl,--no-undefined -Wl,--error-limit=0 -Wl,-Bsymbolic -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
