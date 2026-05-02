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

set "COMMON_FLAGS=/nologo /std:c++20 /EHsc /DUSE_SPIRV_CROSS_LIB /Ivendor\SPIRV-Cross /I. /Icore /Icore\middleware /Iphases\lexing /Iphases\parser /Iphases\evaluation /Iphases\ir_generation /Iphases\ir_lowering /Iphases\control_flow /Iphases\ssa /Iphases\backends\spirv /Iphases\backends\gles"
set "RELEASE_FLAGS=/O2 /W4"
set "DEBUG_FLAGS=/Zi /Od /W4"
set "LINK_FLAGS=/link /STACK:8388608"
set "SOURCES=tools\spirv_cross_wrapper.cpp tools\bwslc.cpp"

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
cl %RELEASE_FLAGS% %COMMON_FLAGS% /Fobuild\ /Febuild\bwslc.exe %SOURCES% %LINK_FLAGS%
if errorlevel 1 exit /b 1
echo Built: build\bwslc.exe
exit /b 0

:build_debug
cl %DEBUG_FLAGS% %COMMON_FLAGS% /Fobuild\ /Fdbuild\bwslc-debug.pdb /Febuild\bwslc-debug.exe %SOURCES% %LINK_FLAGS%
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
where cl >nul 2>&1
if not errorlevel 1 exit /b 0

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

:help
echo Usage:
echo   build.bat [target]
echo.
echo Targets:
echo   bwslc              Build release CLI compiler ^(default^)
echo   bwslc-debug        Build debug CLI compiler
echo   bwslc-msvc         Alias for bwslc
echo   bwslc-msvc-debug   Alias for bwslc-debug
echo   test               Run tests via bash if available
echo   clean              Remove build artifacts
echo   help               Show this message
exit /b 1
