# [CHOSEN, not recovered] BF2Audio.dll as a Ninja target: the same image Code/Build/bf2audio.cmd builds
# (TU manifest Code/Build/bf2audio_sources.txt, flags, defines, link inputs and ABI checks), compiled
# incrementally with compiler-reported header dependencies. The script remains for manual, smoke and x86
# parity builds and reads the same manifest.
#
# Included by the root CMakeLists.txt. Defines bf2_bf2audio_native (OUTPUT_NAME BF2Audio) in
# ${CMAKE_BINARY_DIR}/bf2audio.
include("${PROJECT_SOURCE_DIR}/Code/Build/plugin_common.cmake")
set(_abi "${_code}/BF2AudioAbi")
set(_ogg "${_bf2}/External/libogg-1.3.6")
set(_vorbis "${_bf2}/External/libvorbis-1.3.7")
set(_out "${CMAKE_BINARY_DIR}/bf2audio")
if(NOT EXISTS "${_vorbis}/lib/vorbisfile.c")
    message(FATAL_ERROR "BF2Audio: the vendored Ogg Vorbis sources are missing: ${_vorbis}/lib/vorbisfile.c")
endif()

bf2_read_plugin_manifest(_sources "${PROJECT_SOURCE_DIR}/Code/Build/bf2audio_sources.txt")

# Frontier.cpp, the shared instrument, with this DLL's log file, env var and banner.
set(_frontier "${_code}/RendDX9Abi/Frontier.cpp")
set_source_files_properties("${_frontier}" PROPERTIES COMPILE_DEFINITIONS
    "RD9ABI_LOG_FILENAME=\"BF2AudioRebuilt.log\";RD9ABI_FRONTIER_ENV=\"BF2_BF2AUDIO_FRONTIER\";RD9ABI_LOG_BANNER=\"BF2Audio_replacement\"")

add_library(bf2_bf2audio_native SHARED ${_sources} "${_frontier}" "${_abi}/BF2Audio.def")
bf2_plugin_dll_properties(bf2_bf2audio_native BF2Audio "${_out}" MultiThreadedDLL)

# C++: the script's FLAGS. /DBF2_IMAGE_BF2AUDIO selects the DLL-side ISoundEngine table.
set(_cxx_options /std:c++14 /permissive /EHsc /W3 "/FI${_bf2}/Core/BuildConfig.h"
    /wd4100 /wd4127 /wd4244 /wd4267 /wd4291 /wd4305 /wd4996)
if(BF2_BUILD_X64)
    list(APPEND _cxx_options /we4311 /we4312)
endif()
set(_cxx_defines BF2_IMAGE_BF2AUDIO BF2_PLUGIN_ABI_NATIVE=1 _ITERATOR_DEBUG_LEVEL=0 DEDICATED=0
    WIN32 _WINDOWS NDEBUG _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
if(BF2_FRONTIER_DEFAULT_CONTINUE)
    list(APPEND _cxx_defines BF2_FRONTIER_DEFAULT_CONTINUE=1)
endif()
target_compile_options(bf2_bf2audio_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_options}>")
target_compile_definitions(bf2_bf2audio_native PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_cxx_defines}>")

# C: the script's CFLAGS for the Ogg Vorbis decoder (third-party: no image define, no BuildConfig).
target_compile_options(bf2_bf2audio_native PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:/W3;/wd4018;/wd4101;/wd4146;/wd4244;/wd4267;/wd4305;/wd4700;/wd4996>")
target_compile_definitions(bf2_bf2audio_native PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:WIN32;_WINDOWS;NDEBUG;_CRT_SECURE_NO_WARNINGS;_CRT_NONSTDC_NO_DEPRECATE>")
target_include_directories(bf2_bf2audio_native PRIVATE "${_ogg}/include" "${_vorbis}/include"
    "$<$<COMPILE_LANGUAGE:C>:${_vorbis}/lib>")

# The script's STEP 3. dsound: DirectSoundEnumerateA, the device probe. OpenAL is loaded by name at
# provider open, never linked.
target_link_libraries(bf2_bf2audio_native PRIVATE kernel32.lib user32.lib dsound.lib)
target_link_options(bf2_bf2audio_native PRIVATE /OPT:REF /OPT:ICF)
if(NOT BF2_BUILD_X64)
    target_link_options(bf2_bf2audio_native PRIVATE /BASE:0x10000000)
endif()
bf2_plugin_abicheck(bf2_bf2audio_native bf2audio "${_out}")
