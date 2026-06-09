@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=bwslc"

if /I "%TARGET%"=="help" goto :help
if /I "%TARGET%"=="clean" goto :clean

call :ensure_msvc
if errorlevel 1 exit /b 1

if not exist "build" mkdir "build"

if not defined USE_LINKED_SPIRV_TOOLS set "USE_LINKED_SPIRV_TOOLS=1"

set "SPIRV_TOOLS_FLAGS="
set "SPIRV_TOOLS_LINK_LIB="
if "%USE_LINKED_SPIRV_TOOLS%"=="0" goto :skip_spirv_tools_setup
call :ensure_spirv_tools
if errorlevel 1 exit /b 1
set "SPIRV_TOOLS_FLAGS=/DUSE_SPIRV_TOOLS_LIB /Ivendor\SPIRV-Tools\include /Ivendor\SPIRV-Headers\include"
set "SPIRV_TOOLS_LINK_LIB=%SPIRV_TOOLS_LIB%"
echo Using linked SPIRV-Tools: %SPIRV_TOOLS_LINK_LIB%
:skip_spirv_tools_setup

:: C4324: structure was padded due to alignment specifier (these are intentional in bwslc)
:: C4701: potentially uninitialized local variable (the compiler is too dumb for this to be useful)
:: C4996: strncpy is "unsafe"
set "DISABLED_WARNINGS=/wd4324 /wd4701 /wd4996"
set "INCLUDE_FLAGS=/Ivendor\SPIRV-Cross /I. /Icore /Icore\middleware /Iphases\lexing /Iphases\parser /Iphases\evaluation /Iphases\ir_generation /Iphases\ir_lowering /Iphases\control_flow /Iphases\ssa /Iphases\backends\spirv /Iphases\backends\gles"
set "CPU_FLAGS=/arch:AVX /arch:AVX2"
set "COMMON_FLAGS=/nologo /std:c++20 /EHsc /DUSE_SPIRV_CROSS_LIB %SPIRV_TOOLS_FLAGS% %INCLUDE_FLAGS% %CPU_FLAGS% %DISABLED_WARNINGS%"
set "RELEASE_FLAGS=/O2 /W4 /MD"
set "DEBUG_FLAGS=/Zi /Od /W4 /MDd"
set "LINK_FLAGS=/link /STACK:8388608"
set "WRAPPER_SRC=tools\spirv_cross_wrapper.cpp"
set "BWSLC_SRC=tools\bwslc.cpp"
set "SCCACHE_FLAGS="

set "COMPILER=cl"
where sccache >nul 2>&1
if errorlevel 1 where sccache >nul 2>&1
if not errorlevel 1 (
    set "COMPILER=sccache cl"
    set "SCCACHE_FLAGS=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache"
)

if /I "%TARGET%"=="bwslc" goto :build_release
if /I "%TARGET%"=="release" goto :build_release
if /I "%TARGET%"=="bwslc-msvc" goto :build_release
if /I "%TARGET%"=="bwslc-debug" goto :build_debug
if /I "%TARGET%"=="debug" goto :build_debug
if /I "%TARGET%"=="bwslc-msvc-debug" goto :build_debug
if /I "%TARGET%"=="test" goto :test

echo Unknown target: %TARGET%
goto :help

:build_release
%COMPILER% %RELEASE_FLAGS% %COMMON_FLAGS% /c /Fobuild\spirv_cross_wrapper.obj %WRAPPER_SRC%
if errorlevel 1 exit /b 1
%COMPILER% %RELEASE_FLAGS% %COMMON_FLAGS% /c /Fobuild\bwslc.obj %BWSLC_SRC%
if errorlevel 1 exit /b 1
cl /nologo /Febuild\bwslc.exe build\spirv_cross_wrapper.obj build\bwslc.obj %SPIRV_TOOLS_LINK_LIB% %LINK_FLAGS%
if errorlevel 1 exit /b 1
echo Built: build\bwslc.exe
exit /b 0

:build_debug
%COMPILER% %DEBUG_FLAGS% %COMMON_FLAGS% /c /Fobuild\spirv_cross_wrapper_debug.obj %WRAPPER_SRC%
if errorlevel 1 exit /b 1
%COMPILER% %DEBUG_FLAGS% %COMMON_FLAGS% /c /Fdbuild\bwslc-debug.pdb /Fobuild\bwslc_debug.obj %BWSLC_SRC%
if errorlevel 1 exit /b 1
cl /nologo /Fdbuild\bwslc-debug.pdb /Febuild\bwslc-debug.exe build\spirv_cross_wrapper_debug.obj build\bwslc_debug.obj %SPIRV_TOOLS_LINK_LIB% %LINK_FLAGS%
if errorlevel 1 exit /b 1
echo Built: build\bwslc-debug.exe
exit /b 0

:test
if not exist "build\bwslc.exe" (
    call :build_release
    if errorlevel 1 exit /b 1
)
where python >nul 2>&1
if errorlevel 1 (
    echo Tests require Python on PATH.
    exit /b 1
)
python tests\run_tests.py
exit /b %errorlevel%

:clean
if exist "build" rmdir /S /Q "build"
echo Cleaned build artifacts.
exit /b 0

:ensure_msvc
if defined VSINSTALLDIR (
    if exist "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat" (
        call "%VSINSTALLDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
        if errorlevel 1 (
            echo Failed to initialize the Visual Studio build environment from VSINSTALLDIR.
            exit /b 1
        )
        where cl >nul 2>&1
        if not errorlevel 1 exit /b 0
    )
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo Could not find vswhere.exe.
    echo Install Visual Studio Build Tools with the Desktop C++ workload.
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"

if not defined VSINSTALL (
    echo Could not find a Visual Studio installation with C++ build tools.
    exit /b 1
)

if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Found Visual Studio at:
    echo   %VSINSTALL%
    echo but vcvars64.bat is missing.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Failed to initialize the Visual Studio build environment.
    exit /b 1
)

where cl >nul 2>&1
if errorlevel 1 (
    echo MSVC compiler not found after running vcvars64.bat.
    exit /b 1
)

exit /b 0

:ensure_spirv_tools
where cmake >nul 2>&1
if errorlevel 1 (
    echo Linked SPIRV-Tools validation requires CMake on PATH.
    echo Set USE_LINKED_SPIRV_TOOLS=0 to build with external spirv-val fallback.
    exit /b 1
)

if not exist "vendor\SPIRV-Tools\CMakeLists.txt" (
    echo Missing vendor\SPIRV-Tools. Run: git submodule update --init --recursive
    exit /b 1
)

if not exist "vendor\SPIRV-Headers\include" (
    echo Missing vendor\SPIRV-Headers. Run: git submodule update --init --recursive
    exit /b 1
)

set "SPIRV_TOOLS_BUILD=build\spirv-tools-build"
set "SPIRV_HEADERS_SOURCE_DIR=%CD%\vendor\SPIRV-Headers"
set "SPIRV_HEADERS_SOURCE_DIR=%SPIRV_HEADERS_SOURCE_DIR:\=/%"
cmake -S vendor\SPIRV-Tools -B "%SPIRV_TOOLS_BUILD%" -A x64 -DCMAKE_BUILD_TYPE=Release "-DSPIRV-Headers_SOURCE_DIR=%SPIRV_HEADERS_SOURCE_DIR%" %SCCACHE_FLAGS% -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV_BUILD_FUZZER=OFF
if errorlevel 1 exit /b 1

cmake --build "%SPIRV_TOOLS_BUILD%" --config Release --target SPIRV-Tools-static spirv-val spirv-dis --parallel
if errorlevel 1 exit /b 1

set "SPIRV_TOOLS_LIB="
for %%P in ("%SPIRV_TOOLS_BUILD%\source\Release\SPIRV-Tools.lib" "%SPIRV_TOOLS_BUILD%\source\SPIRV-Tools.lib" "%SPIRV_TOOLS_BUILD%\source\RelWithDebInfo\SPIRV-Tools.lib" "%SPIRV_TOOLS_BUILD%\source\Debug\SPIRV-Tools.lib") do (
    if exist "%%~fP" set "SPIRV_TOOLS_LIB=%%~fP"
)

if not defined SPIRV_TOOLS_LIB (
    echo Could not find SPIRV-Tools.lib under %SPIRV_TOOLS_BUILD%.
    exit /b 1
)

exit /b 0

:help
echo Usage:
echo   build.bat [target]
echo.
echo Targets:
echo   bwslc              Build release CLI compiler ^(default^)
echo   bwslc-debug        Build debug CLI compiler
echo   test               Run tests via Python
echo   clean              Remove build artifacts
echo   help               Show this message
echo.
echo Set USE_LINKED_SPIRV_TOOLS=0 to skip the linked validator and use external tools.
exit /b 1
