# [CHOSEN, not recovered] RendDX9.dll as a Ninja target: the same image Code/Build/renddx9.cmd builds
# (TU manifest, flags, defines, link inputs and ABI checks), compiled incrementally and in parallel with
# compiler-reported header dependencies. The script remains for the G2w seam flavour (BF2_GFX_SEAM) and
# the editor's own RendDX9 (Code/Tools/BF2Editor), and both read the one manifest,
# Code/Build/renddx9_sources.txt.
#
# Included by the root CMakeLists.txt with BF2_PLUGIN_FLAVOUR (x64 | native) set. Defines the target
# bf2_renddx9_native (OUTPUT_NAME RendDX9) in ${CMAKE_BINARY_DIR}/renddx9.

set(_code "${PROJECT_SOURCE_DIR}/Code")
set(_abi "${_code}/RendDX9Abi")
set(_bf2 "${_code}/BF2")
set(_out "${CMAKE_BINARY_DIR}/renddx9")
if(BF2_BUILD_X64)
    set(_machine x64)
else()
    set(_machine x86)
endif()

# The script compiles without optimisation and without NDEBUG (no /O, no config flags), and the
# FFmpeg decoders with exactly their own flags. Clear the configuration flags for this directory so
# the CMake build matches it; the flags each source needs are added below.
foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_CXX_FLAGS${_cfg} "")
    set(CMAKE_C_FLAGS${_cfg} "")
endforeach()
set(CMAKE_CXX_STANDARD_LIBRARIES "")
set(CMAKE_C_STANDARD_LIBRARIES "")

# ---- D3DX: the April 2005 DirectX SDK, or the .def fallback (Code/Build/dxsdk.cmd, same rules) -------
# BF2_D3DX (environment): auto (default) takes the SDK when present, else the fallback; sdk requires
# it; def forces the fallback.
set(_d3dx_mode "$ENV{BF2_D3DX}")
if(NOT _d3dx_mode)
    set(_d3dx_mode auto)
endif()
if(NOT _d3dx_mode MATCHES "^(auto|sdk|def)$")
    message(FATAL_ERROR "RendDX9: BF2_D3DX must be auto, sdk or def (is \"${_d3dx_mode}\")")
endif()
set(_dxsdk "")
if(NOT _d3dx_mode STREQUAL "def")
    set(_candidates)
    if(DEFINED ENV{DXSDK_DIR})
        file(TO_CMAKE_PATH "$ENV{DXSDK_DIR}" _dxsdk_env)
        list(APPEND _candidates "${_dxsdk_env}")
    endif()
    set(_pf86 "ProgramFiles(x86)")
    file(TO_CMAKE_PATH "$ENV{${_pf86}}" _pf86_path)
    file(TO_CMAKE_PATH "$ENV{ProgramFiles}" _pf_path)
    list(APPEND _candidates
        "${_pf86_path}/Microsoft DirectX 9.0 SDK (April 2005)"
        "${_pf_path}/Microsoft DirectX 9.0 SDK (April 2005)")
    foreach(_try IN LISTS _candidates)
        string(REGEX REPLACE "/+$" "" _try "${_try}")
        if(_dxsdk OR NOT EXISTS "${_try}/Include/d3dx9core.h")
            continue()
        endif()
        file(STRINGS "${_try}/Include/d3dx9core.h" _ver REGEX "#define D3DX_SDK_VERSION 25")
        if(_ver AND EXISTS "${_try}/Lib/${_machine}/d3dx9.lib" AND EXISTS "${_try}/Lib/${_machine}/DxErr9.lib")
            set(_dxsdk "${_try}")
        endif()
    endforeach()
    if(NOT _dxsdk AND _d3dx_mode STREQUAL "sdk")
        message(FATAL_ERROR "RendDX9: BF2_D3DX=sdk but no April 2005 DirectX SDK was found (docs/building.md, \"DirectX SDK\")")
    endif()
endif()
if(_dxsdk)
    message(STATUS "RendDX9: DirectX SDK ${_dxsdk} (D3DX 25, ${_machine})")
else()
    message(STATUS "RendDX9: no DirectX SDK - D3DX from Code/RendDX9Abi/D3dx9_25.def and D3dxFallbackInclude")
endif()

# ---- import libraries generated from .def files ------------------------------------------------------
set(_implibs)
function(_renddx9_implib name def)
    add_custom_command(
        OUTPUT "${_out}/lib/${name}.lib"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out}/lib"
        COMMAND "${CMAKE_AR}" /nologo "/def:${def}" "/machine:${_machine}" "/out:${_out}/lib/${name}.lib"
        DEPENDS "${def}"
        COMMENT "RendDX9: ${name}.lib from ${def}"
        VERBATIM)
    set(_implibs ${_implibs} "${_out}/lib/${name}.lib" PARENT_SCOPE)
endfunction()
if(BF2_BUILD_X64)
    _renddx9_implib(TextureAtlasBuilder "${_abi}/TextureAtlasBuilder_x64.def")
else()
    _renddx9_implib(TextureAtlasBuilder "${_abi}/TextureAtlasBuilder.def")
    # binkw32.dll exists only as a 32-bit DLL; x64 links the FFmpeg-based port below instead.
    _renddx9_implib(binkw32 "${_abi}/binkw32.def")
endif()
if(_dxsdk)
    set(_d3dx_lib "${_dxsdk}/Lib/${_machine}/d3dx9.lib")
else()
    _renddx9_implib(d3dx9_25 "${_abi}/D3dx9_25.def")
    set(_d3dx_lib "${_out}/lib/d3dx9_25.lib")
endif()
add_custom_target(bf2_renddx9_implibs DEPENDS ${_implibs})

# ---- sources -----------------------------------------------------------------------------------------
set(_manifest "${PROJECT_SOURCE_DIR}/Code/Build/renddx9_sources.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest}")
file(STRINGS "${_manifest}" _lines)
set(_sources)
foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line AND NOT _line MATCHES "^#")
        list(APPEND _sources "${_code}/${_line}")
    endif()
endforeach()
if(NOT _dxsdk)
    # DXGetErrorDescription9A without the SDK's DxErr9.lib.
    list(APPEND _sources "${_abi}/DxErr9StandIn.cpp")
endif()
set(_ffbink_sources)
if(BF2_BUILD_X64)
    # The Bink stand-in (Code/BF2/External/ffmpeg-bink/README.md): the C++ port with the image flags,
    # FFmpeg 7.1.1's Bink decoders (LGPL-2.1+) as optimised C11.
    list(APPEND _sources "${_abi}/BinkFFmpeg.cpp" "${_abi}/BinkAudioDirectSound.cpp")
    set(_ffb "${_bf2}/External/ffmpeg-bink")
    set(_ffbink_sources
        "${_ffb}/libavcodec/bink.c" "${_ffb}/libavcodec/binkdsp.c" "${_ffb}/libavcodec/binkaudio.c"
        "${_ffb}/libavcodec/wma_freqs.c" "${_ffb}/shim/ffbink_shim.c")
endif()

add_library(bf2_renddx9_native SHARED ${_sources} ${_ffbink_sources} "${_abi}/RendDX9.def")
add_dependencies(bf2_renddx9_native bf2_renddx9_implibs)
set_target_properties(bf2_renddx9_native PROPERTIES
    OUTPUT_NAME RendDX9
    DEFINE_SYMBOL ""
    RUNTIME_OUTPUT_DIRECTORY "${_out}"
    LIBRARY_OUTPUT_DIRECTORY "${_out}"
    ARCHIVE_OUTPUT_DIRECTORY "${_out}/lib"
    PDB_OUTPUT_DIRECTORY "${_out}"
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    FOLDER "BF2")
bf2_add_compile_debug_info(bf2_renddx9_native)

# C++: the script's FLAGS. /DBF2_IMAGE_RENDDX9 admits the module TUs under Code/BF2; /DDEDICATED=0 with
# /FI Core/BuildConfig.h names the client image (AGENTS.md).
set(_cxx_options /std:c++14 /permissive /EHsc /W4 /wd4100 /wd4127 "/FI${_bf2}/Core/BuildConfig.h")
if(BF2_BUILD_X64)
    # A pointer truncated into a 32-bit integer is an error.
    list(APPEND _cxx_options /we4311 /we4312)
endif()
set(_cxx_defines BF2_IMAGE_RENDDX9 BF2_PLUGIN_ABI_NATIVE=1 _ITERATOR_DEBUG_LEVEL=0 DEDICATED=0
    WIN32 _WINDOWS _CRT_SECURE_NO_WARNINGS)
if(BF2_FRONTIER_DEFAULT_CONTINUE)
    list(APPEND _cxx_defines BF2_FRONTIER_DEFAULT_CONTINUE=1)
endif()
set(_cxx_includes)
if(_dxsdk)
    # dxsdk.cmd appends the SDK after the Windows SDK (INCLUDE), so the Windows SDK's d3d9.h wins.
    # /I directories precede INCLUDE, so name the toolchain's directories first, then the SDK's.
    set(_env_include "$ENV{INCLUDE}")
    foreach(_dir IN LISTS _env_include)
        if(_dir)
            file(TO_CMAKE_PATH "${_dir}" _dir)
            list(APPEND _cxx_includes "${_dir}")
        endif()
    endforeach()
    list(APPEND _cxx_includes "${_dxsdk}/Include")
    target_link_directories(bf2_renddx9_native PRIVATE "${_dxsdk}/Lib/${_machine}")
else()
    list(APPEND _cxx_includes "${_abi}/D3dxFallbackInclude")
endif()
target_compile_options(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_options}>")
target_compile_definitions(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_defines}>")
target_include_directories(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_includes}>")

# C: the script's FFBINK_FLAGS.
target_compile_options(bf2_renddx9_native PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:/std:c11;/O2;/W3;/wd4244;/wd4267;/wd4018;/wd4146>")
target_compile_definitions(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:C>:_CRT_SECURE_NO_WARNINGS>")
if(BF2_BUILD_X64)
    target_include_directories(bf2_renddx9_native PRIVATE "$<$<COMPILE_LANGUAGE:C>:${_bf2}/External/ffmpeg-bink/shim>")
endif()

# ---- link: the script's STEP 3 ---------------------------------------------------------------------
# d3dx9_25.dll is delay-loaded, so a missing (or 32-bit only) D3DX reaches BF2Render::init's message box
# instead of failing this DLL's load. TextureAtlasBuilder.dll is a static import, as in retail.
target_link_libraries(bf2_renddx9_native PRIVATE
    "${_d3dx_lib}" ${_implibs} kernel32.lib user32.lib gdi32.lib d3d9.lib delayimp.lib)
target_link_options(bf2_renddx9_native PRIVATE
    /DEBUG /INCREMENTAL:NO /DELAYLOAD:d3dx9_25.dll "/MAP:${_out}/RendDX9.map")
if(NOT _dxsdk)
    # Effect.cpp asks for the SDK's dxerr9.lib by #pragma; the fallback compiles DxErr9StandIn.cpp.
    target_link_options(bf2_renddx9_native PRIVATE /NODEFAULTLIB:dxerr9.lib)
endif()
if(NOT BF2_BUILD_X64)
    # The shipped x86 image base, kept for side-by-side dumpbin comparison.
    target_link_options(bf2_renddx9_native PRIVATE /BASE:0x10000000)
endif()

# ---- the script's STEP 4: the image's ABI invariants, checked on every link ------------------------
get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
find_program(BF2_DUMPBIN dumpbin HINTS "${_linker_dir}" REQUIRED)
if(_dxsdk)
    set(_fallback OFF)
else()
    set(_fallback ON)
endif()
add_custom_command(TARGET bf2_renddx9_native POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DDUMPBIN=${BF2_DUMPBIN}" "-DDLL=$<TARGET_FILE:bf2_renddx9_native>"
            "-DARCH=${_machine}" "-DFALLBACK=${_fallback}" "-DWORK=${_out}/abicheck"
            -P "${PROJECT_SOURCE_DIR}/Code/Build/renddx9_abicheck.cmake"
    VERBATIM)
