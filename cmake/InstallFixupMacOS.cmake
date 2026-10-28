# Runs at `cmake --install` time on macOS (see install(SCRIPT ...) in
# CMakeLists.txt). The linker records libclang's absolute build-time path in the
# binary's load commands; rewrite it to @rpath so the installed shroud loads the
# libclang bundled under @loader_path/../lib, keeping the package relocatable.
set(_bin "${CMAKE_INSTALL_PREFIX}/bin/shroud")
file(GLOB _libclang "${CMAKE_INSTALL_PREFIX}/lib/libclang*.dylib")
if(NOT _libclang OR NOT EXISTS "${_bin}")
    return()
endif()
list(GET _libclang 0 _libclang)
get_filename_component(_libclang_name "${_libclang}" NAME)

# Normalise the bundled dylib's own install name.
execute_process(COMMAND install_name_tool -id "@rpath/${_libclang_name}" "${_libclang}")

# Rewrite each load command in the binary that points at a libclang dylib by an
# absolute (or otherwise non-@rpath) path.
execute_process(COMMAND otool -L "${_bin}"
    OUTPUT_VARIABLE _deps OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" _deps "${_deps}")
foreach(_line IN LISTS _deps)
    string(REGEX REPLACE " \\(compatibility.*$" "" _path "${_line}")
    string(STRIP "${_path}" _path)
    get_filename_component(_name "${_path}" NAME)
    if(_name MATCHES "^libclang.*\\.dylib$" AND NOT _path MATCHES "^@rpath/")
        execute_process(COMMAND install_name_tool -change "${_path}" "@rpath/${_name}" "${_bin}")
    endif()
endforeach()

# Make sure the loader searches lib/ next to the binary. CMake's INSTALL_RPATH
# normally adds this already; -add_rpath then errors harmlessly.
execute_process(COMMAND install_name_tool -add_rpath "@loader_path/../lib" "${_bin}"
    ERROR_QUIET)
