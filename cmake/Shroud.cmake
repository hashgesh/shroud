# CMake glue. Include this file, then wrap the sources you want transformed.
#
#   include(.../cmake/Shroud.cmake)
#   shroud_obfuscate_files(OBF_SOURCES
#       FILES app.cpp util.cpp
#       SEED 0x5f3a1c
#       ARGS -std=c++20 -Iinclude -DNDEBUG)
#   add_executable(app ${OBF_SOURCES})
#   target_include_directories(app PRIVATE ${SHROUD_INCLUDE_DIR})
#
# shroud runs at configure time, so a re-run of the obfuscator needs a re-configure.
# shortcut: configure-time generation keeps this file at ~30 lines; move to
# add_custom_command(OUTPUT ...) when incremental rebuilds start to hurt.

set(SHROUD_EXECUTABLE "shroud" CACHE FILEPATH "shroud binary used by shroud_obfuscate_files")
set(SHROUD_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include" CACHE PATH "runtime header for generated code")

function(shroud_obfuscate_files out_var)
    cmake_parse_arguments(P "" "SEED;MAP" "FILES;ARGS" ${ARGN})
    if(NOT P_SEED)
        set(P_SEED 0)
    endif()
    if(NOT P_MAP)
        set(P_MAP "${CMAKE_BINARY_DIR}/shroud.map")
    endif()

    set(outputs)
    foreach(f IN LISTS P_FILES)
        get_filename_component(abs "${f}" ABSOLUTE)
        get_filename_component(dir "${abs}" DIRECTORY)
        get_filename_component(name "${abs}" NAME_WE)
        get_filename_component(ext "${abs}" EXT)
        set(gen "${dir}/${name}.shroud${ext}")
        execute_process(
            COMMAND "${SHROUD_EXECUTABLE}" obfuscate "${abs}" --out "${gen}" --map "${P_MAP}" --seed ${P_SEED} --
                    ${P_ARGS}
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE log
            ERROR_VARIABLE log)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "shroud failed on ${f}:\n${log}")
        endif()
        list(APPEND outputs "${gen}")
    endforeach()
    set(${out_var} "${outputs}" PARENT_SCOPE)
endfunction()
