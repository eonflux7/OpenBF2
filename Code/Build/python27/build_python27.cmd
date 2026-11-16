@echo off
setlocal EnableDelayedExpansion
rem ===========================================================================
rem SCAFFOLDING - NOT RECOVERED SOURCE. [CHOSEN, not recovered]
rem Builds CPython 2.7.12's core (PCbuild\pythoncore.vcxproj's source list, pythoncore27.files)
rem into dice_py.dll / dice_py.lib for the x64 client with BF2_PYTHON27=ON (Project Reality ships
rem its game scripts as Python 2.7 bytecode).
rem   usage: build_python27.cmd <patched Python-2.7.12 dir> <output dir>
rem The source tree must first be patched with patch_python27.py (UCRT / VS2015+ fixes).
rem Run from an x64 developer environment (Code\Build\vsdevcmd.cmd x64).
rem Keep this file PLAIN ASCII.
rem ===========================================================================
set "PYSRC=%~f1"
set "OUT=%~f2"
if "%PYSRC%"=="" goto :usage
if "%OUT%"=="" goto :usage
if not exist "%PYSRC%\Include\Python.h" ( echo no Python source at %PYSRC% & exit /b 2 )
findstr /c:"dice_py: Universal CRT" "%PYSRC%\Modules\posixmodule.c" >nul || ( echo source not patched - run patch_python27.py first & exit /b 2 )
if not exist "%OUT%" mkdir "%OUT%"
type nul > "%OUT%\objects.rsp"
type nul > "%OUT%\compile.log"

set CFLAGS=/nologo /c /MD /O2 /GF /DWIN32 /D_WINDOWS /DNDEBUG /D_USRDLL /DPy_BUILD_CORE /DPy_ENABLE_SHARED ^
 /DMS_DLL_ID=\"2.7\" /DHGVERSION=\"\" /DHGTAG=\"\" /DHGBRANCH=\"\" ^
 /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /wd4996 /wd4244 /wd4267 /wd4018 /wd4090 ^
 /I"%PYSRC%\Include" /I"%PYSRC%\PC" /I"%PYSRC%\Python" /I"%PYSRC%\Modules\zlib"

echo == compiling CPython 2.7 core into %OUT% ==
set OK=0
set BAD=0
for /f "usebackq delims=" %%F in ("%~dp0pythoncore27.files") do (
    set "REL=%%F"
    set "OBJ=!REL:\=_!"
    set "OBJ=!OBJ:.c=.obj!"
    cl %CFLAGS% /Fo"%OUT%\!OBJ!" "%PYSRC%\%%F" >> "%OUT%\compile.log" 2>&1
    if errorlevel 1 ( set /a BAD+=1 & echo   FAIL %%F ) else ( set /a OK+=1 )
    echo "%OUT%\!OBJ!">> "%OUT%\objects.rsp"
)
echo compiled: !OK!   failed: !BAD!
if not "!BAD!"=="0" ( echo See %OUT%\compile.log & exit /b 1 )

echo == linking dice_py.dll ==
link /nologo /DLL /MACHINE:X64 /out:"%OUT%\dice_py.dll" /IMPLIB:"%OUT%\dice_py.lib" /BASE:0x1E000000 ^
     @"%OUT%\objects.rsp" ws2_32.lib advapi32.lib user32.lib shell32.lib > "%OUT%\link.log" 2>&1
if errorlevel 1 ( type "%OUT%\link.log" & echo dll link failed & exit /b 1 )
for %%A in ("%OUT%\dice_py.dll") do echo    dice_py.dll: %%~zA bytes
exit /b 0

:usage
echo usage: build_python27.cmd ^<patched Python-2.7.12 dir^> ^<output dir^>
exit /b 2
