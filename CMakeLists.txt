cmake_minimum_required(VERSION 3.20)
project(shroud VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libclang is the only dependency. Point LLVM_ROOT at any LLVM >= 16 prefix that
# ships include/clang-c/Index.h and lib/libclang.
set(SHROUD_LLVM_ROOT "$ENV{LLVM_ROOT}" CACHE PATH "LLVM prefix containing include/clang-c and lib/libclang")

find_path(SHROUD_LIBCLANG_INCLUDE clang-c/Index.h
    HINTS ${SHROUD_LLVM_ROOT}/include /usr/lib/llvm-23/include /usr/lib/llvm-22/include /usr/lib/llvm-21/include
          $ENV{HOME}/tools/llvm23/include)
find_library(SHROUD_LIBCLANG clang
    HINTS ${SHROUD_LLVM_ROOT}/lib /usr/lib/llvm-23/lib /usr/lib/llvm-22/lib /usr/lib/llvm-21/lib
          $ENV{HOME}/tools/llvm23/lib)
if(NOT SHROUD_LIBCLANG_INCLUDE OR NOT SHROUD_LIBCLANG)
    message(FATAL_ERROR "libclang not found. Set LLVM_ROOT=/path/to/llvm or -DSHROUD_LLVM_ROOT=...")
endif()

add_executable(shroud
    src/main.cpp
    src/mapfile.cpp
    src/obfuscate.cpp
    src/verify.cpp)
target_include_directories(shroud PRIVATE src include ${SHROUD_LIBCLANG_INCLUDE})
target_link_libraries(shroud PRIVATE ${SHROUD_LIBCLANG})

# The prebuilt LLVM tarballs are not on the default loader path, and libclang
# only finds its builtin headers (stddef.h, ...) through -resource-dir.
get_filename_component(SHROUD_LIBCLANG_DIR "${SHROUD_LIBCLANG}" DIRECTORY)
set_target_properties(shroud PROPERTIES
    BUILD_RPATH "${SHROUD_LIBCLANG_DIR}"
    INSTALL_RPATH "${SHROUD_LIBCLANG_DIR}")

get_filename_component(_shroud_llvm_prefix "${SHROUD_LIBCLANG_INCLUDE}" DIRECTORY)
file(GLOB _shroud_resource_dirs "${_shroud_llvm_prefix}/lib/clang/*")
if(_shroud_resource_dirs)
    list(SORT _shroud_resource_dirs)
    list(GET _shroud_resource_dirs -1 _shroud_resource_dir)
    target_compile_definitions(shroud PRIVATE SHROUD_RESOURCE_DIR="${_shroud_resource_dir}")
endif()

enable_testing()
add_test(NAME smoke COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh $<TARGET_FILE:shroud>)
set_tests_properties(smoke PROPERTIES
    ENVIRONMENT "SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR};BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/test")

# Unit tests for pure logic that doesn't need a real libclang parse (escape
# decoding, the #if scan, edit assembly, map save/load, verify's boundary
# checks). Each tests/unit/test_*.cpp #includes its src/*.cpp counterpart
# directly to reach its anonymous-namespace helpers without exposing them;
# an unnamed namespace has internal linkage, so merging that file's symbols
# into this TU cannot collide with another test_*.cpp doing the same for a
# different source file. See tests/run.sh for the end-to-end checks.
add_executable(unit_tests
    tests/unit/main.cpp
    tests/unit/test_mapfile.cpp
    tests/unit/test_verify.cpp
    tests/unit/test_obfuscate.cpp)
target_include_directories(unit_tests PRIVATE src include ${SHROUD_LIBCLANG_INCLUDE})
target_link_libraries(unit_tests PRIVATE ${SHROUD_LIBCLANG})
set_target_properties(unit_tests PROPERTIES
    BUILD_RPATH "${SHROUD_LIBCLANG_DIR}")
add_test(NAME unit COMMAND unit_tests)
