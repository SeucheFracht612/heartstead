@echo off
setlocal

set "PROJECT_ROOT=%~dp0"
set "GAME_EXE=%PROJECT_ROOT%build-windows-ninja\heartstead.exe"

echo Checking for Heartstead updates...
call "%PROJECT_ROOT%scripts\build-windows.cmd" --no-tests
if errorlevel 1 exit /b %errorlevel%

start "Heartstead" /D "%PROJECT_ROOT%" "%GAME_EXE%"
