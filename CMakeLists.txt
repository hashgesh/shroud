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
# BUILD_RPATH points the build tree at the LLVM prefix so the tests run in
# place. INSTALL_RPATH is relative instead, so an installed package finds the
# bundled libclang in lib/ next to the binary on any machine (see install()
# below). libclang's own RUNPATH is $ORIGIN/../lib, so a libLLVM bundled in the
# same lib/ is found relative to libclang too. Windows has no RPATH: the DLL
# sits beside the .exe in bin/.
if(APPLE)
    set(_shroud_install_rpath "@loader_path/../lib")
elseif(UNIX)
    set(_shroud_install_rpath "$ORIGIN/../lib")
else()
    set(_shroud_install_rpath "")
endif()
set_target_properties(shroud PROPERTIES
    BUILD_RPATH "${SHROUD_LIBCLANG_DIR}"
    INSTALL_RPATH "${_shroud_install_rpath}")

get_filename_component(_shroud_llvm_prefix "${SHROUD_LIBCLANG_INCLUDE}" DIRECTORY)
file(GLOB _shroud_resource_dirs "${_shroud_llvm_prefix}/lib/clang/*")
if(_shroud_resource_dirs)
    list(SORT _shroud_resource_dirs)
    list(GET _shroud_resource_dirs -1 _shroud_resource_dir)
    target_compile_definitions(shroud PRIVATE SHROUD_RESOURCE_DIR="${_shroud_resource_dir}")
endif()

# ---------------------------------------------------------------- install
# A relocatable package: `cmake --install build --prefix dist` produces a tree
# that runs on another machine with no LLVM installed.
#
#   bin/shroud(.exe)
#   lib/                        libclang (+ libLLVM it depends on)   [not Windows]
#   lib/clang/<ver>/include/    clang builtin headers (-resource-dir)
#   include/shroud/runtime.hpp  header injected into obfuscated output
#   cmake/Shroud.cmake          downstream CMake glue
#   README.md, LICENSE
#
# The two things that pinned a release to its build machine -- the libclang
# RPATH and the baked resource-dir -- are both made relative above and here.

install(TARGETS shroud RUNTIME DESTINATION bin)

if(WIN32)
    # On Windows find_library resolves the import lib; the loader needs the DLL
    # beside the .exe. LLVM ships it in <prefix>/bin.
    get_filename_component(_shroud_llvm_bin "${SHROUD_LIBCLANG_DIR}/../bin" ABSOLUTE)
    find_file(SHROUD_LIBCLANG_DLL NAMES libclang.dll
        HINTS "${SHROUD_LLVM_ROOT}/bin" "${_shroud_llvm_bin}")
    if(NOT SHROUD_LIBCLANG_DLL)
        message(WARNING "libclang.dll not found; the installed package will not be self-contained")
    else()
        install(PROGRAMS "${SHROUD_LIBCLANG_DLL}" DESTINATION bin)
    endif()
else()
    # Bundle libclang and the libLLVM it depends on into lib/, resolving the
    # soname/linker symlink chain so the name recorded in the binary's
    # DT_NEEDED resolves there. Distro packages split libLLVM out of libclang;
    # the official tarballs link it statically (nothing to glob). file(COPY)
    # with FOLLOW_SYMLINK_CHAIN recreates each symlink next to the real object;
    # install(FILES FOLLOW_SYMLINK_CHAIN) would do the same but needs CMake 3.22,
    # above this project's floor. file() runs at install time, hence install(CODE).
    set(_shroud_runtime_libs "${SHROUD_LIBCLANG}")
    file(GLOB _shroud_libllvm
        "${SHROUD_LIBCLANG_DIR}/libLLVM*.so*"
        "${SHROUD_LIBCLANG_DIR}/libLLVM*.dylib")
    # libclang links the full libLLVM, not the C-API-only libLLVM-C; dropping it
    # avoids shipping a second large dylib nothing loads (seen on Homebrew LLVM).
    list(FILTER _shroud_libllvm EXCLUDE REGEX "libLLVM-C")
    list(APPEND _shroud_runtime_libs ${_shroud_libllvm})
    install(CODE "
        foreach(_lib ${_shroud_runtime_libs})
            message(STATUS \"Bundling runtime library: \${_lib}\")
            file(COPY \"\${_lib}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/lib\"
                 FOLLOW_SYMLINK_CHAIN)
        endforeach()
    ")
    # The loader resolves DT_NEEDED by ELF SONAME, which can differ from the
    # real file's name: apt.llvm.org ships libLLVM-18.so.1 whose SONAME is
    # libLLVM-18.so.18.1, the name libclang records. file(COPY) only recreates
    # pre-existing symlinks, so on a machine with no system LLVM that SONAME has
    # no file. For each bundled real object, read its SONAME and add a
    # <soname> -> <file> symlink when it is missing. (ELF only; macOS dylibs use
    # install names, handled by InstallFixupMacOS.cmake.)
    if(NOT APPLE)
        install(CODE [=[
            file(GLOB _objs "${CMAKE_INSTALL_PREFIX}/lib/libLLVM*.so*"
                            "${CMAKE_INSTALL_PREFIX}/lib/libclang*.so*")
            foreach(_obj IN LISTS _objs)
                if(IS_SYMLINK "${_obj}")
                    continue()
                endif()
                execute_process(
                    COMMAND readelf -d "${_obj}"
                    COMMAND awk "-F[][]" "/SONAME/ {print $2}"
                    OUTPUT_VARIABLE _soname OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
                get_filename_component(_dir "${_obj}" DIRECTORY)
                get_filename_component(_base "${_obj}" NAME)
                if(_soname AND NOT _soname STREQUAL _base AND NOT EXISTS "${_dir}/${_soname}")
                    message(STATUS "shroud: SONAME link ${_soname} -> ${_base}")
                    execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
                                    "${_base}" "${_dir}/${_soname}")
                endif()
            endforeach()
        ]=])
    endif()
endif()

# Builtin headers -> lib/clang/<ver>/include, which the binary resolves via
# <exe>/../lib/clang/<ver>/include at runtime (see src/obfuscate.cpp).
if(_shroud_resource_dir AND EXISTS "${_shroud_resource_dir}/include")
    get_filename_component(_shroud_resource_ver "${_shroud_resource_dir}" NAME)
    install(DIRECTORY "${_shroud_resource_dir}/include"
        DESTINATION "lib/clang/${_shroud_resource_ver}")
else()
    message(WARNING "clang resource-dir not found; the installed package will not be self-contained")
endif()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/shroud/runtime.hpp"
    DESTINATION include/shroud)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Shroud.cmake" DESTINATION cmake)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION .)
foreach(_doc LICENSE LICENSE.txt LICENSE.md)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}" DESTINATION .)
    endif()
endforeach()

# libclang/libLLVM are bundled into the package; ship their license alongside ours.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/LLVM-LICENSE.TXT"
    DESTINATION . RENAME LICENSE.LLVM.txt)

# macOS records libclang's load path in the binary; rewrite it to @rpath so the
# bundled copy under @loader_path/../lib is used instead of the build-time path.
if(APPLE)
    install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallFixupMacOS.cmake")
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
