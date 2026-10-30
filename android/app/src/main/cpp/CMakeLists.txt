cmake_minimum_required(VERSION 3.22)
project(frond C CXX)

# FROND_CORE is the reconstructed core (build/source), SDL2_SOURCE and
# SDL2_IMAGE_SOURCE the pinned SDL2 and SDL2_image trees.
if(NOT IS_DIRECTORY "${FROND_CORE}/src/uarm" OR NOT IS_DIRECTORY "${SDL2_SOURCE}"
        OR NOT IS_DIRECTORY "${SDL2_IMAGE_SOURCE}")
    message(FATAL_ERROR "Run the prepareCore, fetchSdl and fetchSdlImage Gradle tasks first")
endif()
option(FROND_SELF_TEST "Build the M1 CPU self-test as libmain.so instead of the emulator" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)

# Debug builds optimise all native code like the desktop's -O2 -g, SDL included, so
# speed measured on a debug build holds for release.
string(APPEND CMAKE_C_FLAGS_DEBUG " -O2")
string(APPEND CMAKE_CXX_FLAGS_DEBUG " -O2")

# Debug builds would turn on SDL_assert, and a failed one blocks on a dialog
# (Android's audio enumeration trips one on capture devices).
set(SDL_ASSERTIONS "release" CACHE STRING "" FORCE)
add_subdirectory("${SDL2_SOURCE}" sdl2)

# SDL2_image only needs PNG: stb_image loads it and the built-in miniz saves it.
foreach(format AVIF BMP GIF JPG JXL LBM PCX PNM QOI SVG TGA TIF WEBP XCF XPM XV)
    set(SDL2IMAGE_${format} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL2IMAGE_PNG ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL2_IMAGE_SOURCE}" sdl2_image)

# Read the source lists from the core's own Makefiles so patches that add files
# are picked up here too.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/print.mk" "frond-print-%:\n\t@echo $($*)\n")
function(core_sources dir variable result)
    execute_process(
        COMMAND make -s --no-print-directory -C "${dir}"
            -f Makefile -f "${CMAKE_CURRENT_BINARY_DIR}/print.mk" "frond-print-${variable}"
        OUTPUT_VARIABLE list OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    separate_arguments(list UNIX_COMMAND "${list}")
    list(TRANSFORM list PREPEND "${dir}/")
    set(${result} ${list} PARENT_SCOPE)
endfunction()

set(UARM "${FROND_CORE}/src/uarm")
set(COMMON "${FROND_CORE}/src/common")
core_sources("${UARM}" SOURCE_C_NATIVE uarm_c)
core_sources("${UARM}" SOURCE_CXX_NATIVE uarm_cxx)
core_sources("${COMMON}" SOURCE_C common_c)
core_sources("${COMMON}" SOURCE_CXX common_cxx)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${UARM}/Makefile" "${COMMON}/Makefile" "${FROND_CORE}/.prepared")

# The SDL front end in native/ builds into libmain.so with the Android entry point.
set(front_end ${uarm_cxx})
list(FILTER front_end INCLUDE REGEX "/native/")
list(FILTER uarm_cxx EXCLUDE REGEX "/native/")

add_library(frondcore STATIC ${uarm_c} ${uarm_cxx} ${common_c} ${common_cxx})
target_include_directories(frondcore PUBLIC
    "${UARM}/uarm" "${UARM}" "${COMMON}" "${COMMON}/zip" "${FROND_CORE}/src/argparse")
set(network "${FROND_NETWORK_ROOT}/${ANDROID_ABI}/install")
add_library(slirp STATIC IMPORTED)
set_target_properties(slirp PROPERTIES
    IMPORTED_LOCATION "${network}/lib/libslirp.a"
    INTERFACE_INCLUDE_DIRECTORIES "${network}/include/slirp")
target_link_libraries(frondcore PUBLIC slirp
    "${network}/lib/libglib-2.0.a" "${network}/lib/libpcre2-8.a" "${network}/lib/libiconv.a" "${network}/lib/libintl.a" m)
target_compile_definitions(frondcore PUBLIC _FILE_OFFSET_BITS=64)
# The core is too slow to use at -O0, so debug builds optimise it as well.
target_compile_options(frondcore PRIVATE -O2 -g
    -Wno-unused-parameter -Wno-unknown-pragmas -Wno-visibility
    -Wno-constant-conversion -Wno-constant-logical-operand)

if(FROND_SELF_TEST)
    # The CPU tests from tests/ run as the synthetic self-test.
    add_library(main SHARED self_test.cpp test_arm_alignment.cpp test_instruction_cache.cpp)
    target_include_directories(main PRIVATE "${FROND_CORE}/../../tests")
    target_compile_options(main PRIVATE -O2)
    target_link_libraries(main PRIVATE frondcore SDL2 SDL2main log)
    return()
endif()

# The desktop front end (what out/frond runs) with the readline CLI swapped for a
# stub. Its main() is renamed so android_main.cpp can set up the process first.
add_library(main SHARED android_main.cpp CliNone.cpp ${front_end})
target_include_directories(main PRIVATE "${UARM}/native")
set_source_files_properties("${UARM}/native/main.cpp" PROPERTIES
    COMPILE_DEFINITIONS "SDL_MAIN_HANDLED;main=frond_core_main")
target_compile_options(main PRIVATE -O2 -g
    -Wno-unused-parameter -Wno-unknown-pragmas -Wno-visibility
    -Wno-constant-conversion -Wno-constant-logical-operand)
target_link_libraries(main PRIVATE frondcore SDL2 SDL2main SDL2_image log)
