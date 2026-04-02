@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
cd /d "%ROOT%"

where make.exe >nul 2>&1
if not errorlevel 1 (
    make.exe %*
    exit /b %errorlevel%
)

call "%ROOT%build.bat" %*
exit /b %errorlevel%
