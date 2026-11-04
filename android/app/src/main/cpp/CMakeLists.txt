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

# The Fast engine: Dynarmic on arm64-v8a (its arm64 back end) and x86_64, from FROND_DYNARMIC,
# the patched source make dynarmic-source exports. Other ABIs build with the interpreter only.
if(FROND_DYNARMIC AND ANDROID_ABI MATCHES "^(arm64-v8a|x86_64)$")
    # Dynarmic only needs Boost's header-only parts; FROND_BOOST_INCLUDE holds the host's boost/.
    set(boost "${CMAKE_CURRENT_BINARY_DIR}/boost")
    file(MAKE_DIRECTORY "${boost}/include")
    file(CREATE_LINK "${FROND_BOOST_INCLUDE}/boost" "${boost}/include/boost" SYMBOLIC)
    file(WRITE "${boost}/BoostConfig.cmake"
        "if(NOT TARGET Boost::boost)\n"
        "  add_library(Boost::boost INTERFACE IMPORTED)\n"
        "  set_target_properties(Boost::boost PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${boost}/include\")\n"
        "endif()\n")
    file(STRINGS "${FROND_BOOST_INCLUDE}/boost/version.hpp" boost_version REGEX "define BOOST_LIB_VERSION")
    string(REGEX REPLACE ".*\"([0-9]+)_([0-9]+).*" "\\1.\\2" boost_version "${boost_version}")
    file(WRITE "${boost}/BoostConfigVersion.cmake"
        "set(PACKAGE_VERSION ${boost_version})\n"
        "if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)\n"
        "  set(PACKAGE_VERSION_COMPATIBLE FALSE)\n"
        "else()\n"
        "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
        "endif()\n")
    set(Boost_DIR "${boost}" CACHE PATH "" FORCE)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
    set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
    set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
    set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
    # Static, linked into libmain.so (SDL turns BUILD_SHARED_LIBS on).
    set(shared_libs ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${FROND_DYNARMIC}" dynarmic EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${shared_libs})
    # The JIT file is C++20 and sees only Dynarmic's headers: the core's include folders hide
    # system ones (uarm/syscall.h).
    add_library(frondjit OBJECT "${UARM}/uarm/jit_dynarmic.cpp")
    set_target_properties(frondjit PROPERTIES CXX_STANDARD 20)
    target_include_directories(frondjit PRIVATE "${boost}/include"
        "${FROND_DYNARMIC}/externals/mcl/include" "${FROND_DYNARMIC}/externals/fmt/include")
    target_compile_options(frondjit PRIVATE -O2 -g)
    target_link_libraries(frondjit PRIVATE dynarmic)
    target_sources(frondcore PRIVATE $<TARGET_OBJECTS:frondjit>)
    # PUBLIC: icache.h and icache_internal.h change with it.
    target_compile_definitions(frondcore PUBLIC FROND_DYNARMIC)
    target_link_libraries(frondcore PRIVATE dynarmic)
endif()

# The JIT differential test (tests/jit-differential.cpp) and benchmark-replay as command-line
# programs, built with -Pfrond.jitDifferential=true, for adb push to /data/local/tmp.
option(FROND_JIT_DIFFERENTIAL "Also build the jit-differential program" OFF)
if(FROND_JIT_DIFFERENTIAL AND TARGET dynarmic)
    add_executable(jit-differential "${FROND_CORE}/../../tests/jit-differential.cpp")
    target_compile_options(jit-differential PRIVATE -O2 -g)
    target_link_libraries(jit-differential PRIVATE frondcore dynarmic log)
    # tools/benchmark-replay.cpp too, for Bike and FrondBench speed and hashes on a device.
    add_executable(benchmark-replay "${FROND_CORE}/../../tools/benchmark-replay.cpp")
    target_include_directories(benchmark-replay PRIVATE "${FROND_CORE}/../../tools")
    target_compile_definitions(benchmark-replay PRIVATE SDL_MAIN_HANDLED)
    target_compile_options(benchmark-replay PRIVATE -O2 -g)
    target_link_libraries(benchmark-replay PRIVATE frondcore SDL2 SDL2_image log)
endif()

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
