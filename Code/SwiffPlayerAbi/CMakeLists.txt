# [BUILD] SwiffPlayer.dll as a Ninja target: the same image Code/Build/swiffplayer.cmd
# builds (TU manifest Code/Build/swiffplayer_sources.txt, flags, defines, link inputs and ABI checks),
# compiled incrementally with compiler-reported header dependencies. The script remains for manual and
# x86 parity builds and reads the same manifest.
#
# Included by the root CMakeLists.txt. Defines bf2_swiffplayer_native (OUTPUT_NAME SwiffPlayer) in
# ${CMAKE_BINARY_DIR}/swiffplayer.
include("${PROJECT_SOURCE_DIR}/Code/Build/plugin_common.cmake")
set(_abi "${_code}/SwiffPlayerAbi")
set(_ext "${_bf2}/External")
set(_out "${CMAKE_BINARY_DIR}/swiffplayer")

bf2_read_plugin_manifest(_all "${PROJECT_SOURCE_DIR}/Code/Build/swiffplayer_sources.txt")
set(_sources ${_all})
list(FILTER _sources EXCLUDE REGEX "\\.c$")
set(_expat_sources ${_all})
list(FILTER _expat_sources INCLUDE REGEX "\\.c$")

# The owner-approved Expat 1.95.0 substitute, compiled as C into a static library as the script does.
add_library(bf2_swiffplayer_expat STATIC ${_expat_sources})
bf2_plugin_optimize(bf2_swiffplayer_expat)
set_target_properties(bf2_swiffplayer_expat PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${_out}/lib"
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    FOLDER "BF2")
bf2_add_compile_debug_info(bf2_swiffplayer_expat)
target_compile_options(bf2_swiffplayer_expat PRIVATE /TC /W3 "/FI${_bf2}/Core/BuildConfig.h")
target_compile_definitions(bf2_swiffplayer_expat PRIVATE DEDICATED=0 _CRT_SECURE_NO_WARNINGS)
target_include_directories(bf2_swiffplayer_expat PRIVATE "${_ext}/expat")

# Frontier.cpp, the shared instrument, with the SwiffPlayer log file, env var and banner.
set(_frontier "${_code}/RendDX9Abi/Frontier.cpp")
set_source_files_properties("${_frontier}" PROPERTIES COMPILE_DEFINITIONS
    "RD9ABI_LOG_FILENAME=\"SwiffPlayerRebuilt.log\";RD9ABI_FRONTIER_ENV=\"BF2_SWIFFPLAYER_FRONTIER\";RD9ABI_LOG_BANNER=\"SwiffPlayer_replacement\"")

add_library(bf2_swiffplayer_native SHARED ${_sources} "${_frontier}" "${_abi}/SwiffPlayer.def")
bf2_plugin_dll_properties(bf2_swiffplayer_native SwiffPlayer "${_out}" MultiThreadedDLL)
# The script's FLAGS. External is THE include root (gameswf and base include as "base/xxx.h"); the
# wrapper TUs include the parent's "gameswf.h". /DNDEBUG matches the shipped release build: gameswf's
# asserts are off, as in retail.
target_compile_options(bf2_swiffplayer_native PRIVATE
    /std:c++14 /permissive /EHsc /W3 "/FI${_bf2}/Core/BuildConfig.h"
    /wd4100 /wd4127 /wd4244 /wd4267 /wd4291 /wd4305 /wd4996)
if(BF2_BUILD_X64)
    target_compile_options(bf2_swiffplayer_native PRIVATE /we4311 /we4312)
endif()
target_compile_definitions(bf2_swiffplayer_native PRIVATE
    BF2_IMAGE_SWIFFPLAYER BF2_PLUGIN_ABI_NATIVE=1 _ITERATOR_DEBUG_LEVEL=0 DEDICATED=0
    WIN32 _WINDOWS NDEBUG _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
if(BF2_FRONTIER_DEFAULT_CONTINUE)
    target_compile_definitions(bf2_swiffplayer_native PRIVATE BF2_FRONTIER_DEFAULT_CONTINUE=1)
endif()
target_include_directories(bf2_swiffplayer_native PRIVATE "${_ext}" "${_ext}/gameswf")

# The script's STEP 3. /OPT:REF drops gameswf's unused OpenGL debug helpers (the shipped image imports no
# opengl32) and /OPT:ICF folds the identical IBase bodies. winmm: timeGetTime for tu_timer.
target_link_libraries(bf2_swiffplayer_native PRIVATE bf2_swiffplayer_expat kernel32.lib user32.lib winmm.lib)
target_link_options(bf2_swiffplayer_native PRIVATE /OPT:REF /OPT:ICF)
if(NOT BF2_BUILD_X64)
    target_link_options(bf2_swiffplayer_native PRIVATE /BASE:0x10000000)
endif()
bf2_plugin_abicheck(bf2_swiffplayer_native swiffplayer "${_out}")
