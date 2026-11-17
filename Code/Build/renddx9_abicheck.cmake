# [CHOSEN, not recovered] RendDX9.dll's native ABI invariants, checked after every link of the CMake
# target bf2_renddx9_native (Code/RendDX9Abi/CMakeLists.txt). The same checks as renddx9.cmd's STEP 4:
# a drifted shape loads and then misbehaves instead of failing, so each one stops the build.
#   cmake -DDUMPBIN=<dumpbin.exe> -DDLL=<RendDX9.dll> -DARCH=x64|x86 -DFALLBACK=ON|OFF -DWORK=<dir>
#         -P Code/Build/renddx9_abicheck.cmake
foreach(_v DUMPBIN DLL ARCH FALLBACK WORK)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "renddx9_abicheck.cmake: -D${_v} is required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${WORK}")

function(_dumpbin out)
    execute_process(COMMAND "${DUMPBIN}" /nologo ${ARGN} "${DLL}"
        OUTPUT_VARIABLE _text RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "renddx9_abicheck: dumpbin ${ARGN} failed (${_rc})")
    endif()
    string(REPLACE "/" "_" _name "${ARGN}")
    string(REPLACE ":" "_" _name "${_name}")
    file(WRITE "${WORK}/${_name}.txt" "${_text}")
    set(${out} "${_text}" PARENT_SCOPE)
endfunction()

set(_bad 0)
macro(_fail msg)
    message(SEND_ERROR "RendDX9 ABI: ${msg}")
    math(EXPR _bad "${_bad} + 1")
endmacro()
macro(_require text needle msg)
    string(FIND "${${text}}" "${needle}" _at)
    if(_at EQUAL -1)
        _fail("${msg}")
    endif()
endmacro()
macro(_forbid text needle msg)
    string(FIND "${${text}}" "${needle}" _at)
    if(NOT _at EQUAL -1)
        _fail("${msg}")
    endif()
endmacro()

_dumpbin(_exports /exports)
_dumpbin(_deps /dependents)
string(TOLOWER "${_deps}" _deps_lower)
_dumpbin(_tab /imports:TextureAtlasBuilder.dll)

# The host resolves both entry points by name.
_require(_exports "initDll" "export initDll MISSING")
_require(_exports "deinitDll" "export deinitDll MISSING")
# Native ABI: no retail heap or CRT, the shared native CRT instead.
_forbid(_deps_lower "memory.dll" "retail heap Memory.dll imported")
_forbid(_deps_lower "msvcr71.dll" "retail CRT msvcr71.dll imported")
_forbid(_deps_lower "msvcp71.dll" "retail CRT msvcp71.dll imported")
_require(_deps_lower "vcruntime140.dll" "native shared VCRUNTIME140.dll MISSING")
string(REGEX MATCH "api-ms-win-crt-heap-|ucrtbase\\.dll" _heap "${_deps_lower}")
if(NOT _heap)
    _fail("native shared CRT heap MISSING")
endif()
# Retail imports d3dx9_25.dll; another D3DX DLL means another effect compiler. Delay-loaded here.
_require(_deps_lower "d3dx9_25.dll" "d3dx9_25.dll MISSING - D3DX must be the April 2005 generation")
_require(_deps_lower "delay load dependencies" "d3dx9_25.dll NOT delay-loaded")
if(FALLBACK)
    # The fallback's import names must be d3dx9_25.dll's plain export names: a decorated name links and
    # then fails to load with ERROR_PROC_NOT_FOUND.
    _dumpbin(_d3dx /imports:d3dx9_25.dll)
    string(REGEX MATCH "D3DX[A-Za-z0-9_]*@" _decorated "${_d3dx}")
    if(_decorated)
        _fail("d3dx9_25.dll import names DECORATED (${_decorated}...)")
    endif()
endif()
# The shipped image imports exactly TextureAtlasBuilder.dll's two creates, by name.
if(ARCH STREQUAL "x64")
    _require(_tab "?create@ITextureAtlas@textureatlas@@SAPEAV12@PEAUIDirect3DDevice9@@@Z" "TextureAtlasBuilder.dll ITextureAtlas::create MISSING")
    _require(_tab "?create@ITextureAtlasConfiguration@textureatlas@@SAPEAV12@XZ" "TextureAtlasBuilder.dll ITextureAtlasConfiguration::create MISSING")
else()
    _require(_tab "?create@ITextureAtlas@textureatlas@@SIPAV12@PAUIDirect3DDevice9@@@Z" "TextureAtlasBuilder.dll ITextureAtlas::create MISSING")
    _require(_tab "?create@ITextureAtlasConfiguration@textureatlas@@SIPAV12@XZ" "TextureAtlasBuilder.dll ITextureAtlasConfiguration::create MISSING")
endif()

if(NOT _bad EQUAL 0)
    file(REMOVE "${DLL}")
    message(FATAL_ERROR "RendDX9 ABI check FAILED (${_bad}); the DLL was removed so it cannot be staged")
endif()
message(STATUS "RendDX9 ABI check: OK")
