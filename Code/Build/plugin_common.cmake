# [CHOSEN, not recovered] Shared setup of the native plugin DLL targets (Code/RendDX9Abi,
# Code/SwiffPlayerAbi, Code/BF2AudioAbi), included at the top of each directory's CMakeLists.txt. Each
# target builds the same image as its Code/Build/<name>.cmd script, from the same TU manifest, so the
# directory takes no compiler flag the script does not pass: the configuration flags are cleared here and
# each target adds its script's flags.

set(_code "${PROJECT_SOURCE_DIR}/Code")
set(_bf2 "${_code}/BF2")
if(BF2_BUILD_X64)
    set(_machine x64)
else()
    set(_machine x86)
endif()

foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_CXX_FLAGS${_cfg} "")
    set(CMAKE_C_FLAGS${_cfg} "")
endforeach()
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_C_STANDARD_LIBRARIES "")

get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
find_program(BF2_DUMPBIN dumpbin HINTS "${_linker_dir}" REQUIRED)

# Reads a TU manifest (one path per line relative to Code/, # comments) into <out>, in file order; a
# manifest edit reconfigures.
function(bf2_read_plugin_manifest out manifest)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
    file(STRINGS "${manifest}" _lines)
    set(_list)
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line AND NOT _line MATCHES "^#")
            list(APPEND _list "${PROJECT_SOURCE_DIR}/Code/${_line}")
        endif()
    endforeach()
    set(${out} ${_list} PARENT_SCOPE)
endfunction()

# The common shape of a plugin DLL target: no export macro, output (DLL, PDB, map, import library) in
# <dir>, embedded (x64) or /Zi debug information, /DEBUG link without incremental linking.
function(bf2_plugin_dll_properties target name dir crt)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${name}
        DEFINE_SYMBOL ""
        RUNTIME_OUTPUT_DIRECTORY "${dir}"
        LIBRARY_OUTPUT_DIRECTORY "${dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${dir}/lib"
        PDB_OUTPUT_DIRECTORY "${dir}"
        MSVC_RUNTIME_LIBRARY ${crt}
        FOLDER "BF2")
    bf2_add_compile_debug_info(${target})
    target_link_options(${target} PRIVATE /DEBUG /INCREMENTAL:NO "/MAP:${dir}/${name}.map")
endfunction()

# The script's STEP 4 on every link: Code/Build/plugin_abicheck.cmake removes the DLL and fails the build
# when an ABI invariant does not hold. Extra arguments are passed through as -D definitions.
function(bf2_plugin_abicheck target plugin dir)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DPLUGIN=${plugin}"
                "-DDUMPBIN=${BF2_DUMPBIN}" "-DDLL=$<TARGET_FILE:${target}>"
                "-DARCH=${_machine}" "-DWORK=${dir}/abicheck" ${ARGN}
                -P "${PROJECT_SOURCE_DIR}/Code/Build/plugin_abicheck.cmake"
        VERBATIM)
endfunction()
