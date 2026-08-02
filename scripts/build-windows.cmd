@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer was not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo Install the Desktop development with C++ workload in Visual Studio Installer.
    exit /b 1
)

call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

set "NINJA=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if exist "%NINJA%" (
    cmake -S "%~dp0.." -B "%~dp0..\build-windows-ninja" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release
) else (
    cmake -S "%~dp0.." -B "%~dp0..\build-windows-ninja" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 exit /b %errorlevel%

cmake --build "%~dp0..\build-windows-ninja"
if errorlevel 1 exit /b %errorlevel%

if /I "%~1"=="--no-tests" exit /b 0

ctest --test-dir "%~dp0..\build-windows-ninja" --output-on-failure
exit /b %errorlevel%
