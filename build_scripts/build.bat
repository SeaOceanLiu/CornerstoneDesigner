@echo off
call "d:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64

set DESIGNER_ROOT=%~dp0..

set BACKEND=%1
if "%BACKEND%"=="" set BACKEND=sdl3

set BUILD_DIR=%DESIGNER_ROOT%\build\%BACKEND%

echo Building CornerstoneDesigner [%BACKEND% backend]...

cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Debug -DUICORNERSTONE_BACKEND=%BACKEND%
if %errorlevel% neq 0 (
    echo CMake configuration failed!
    pause
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config Debug

echo.
echo Build completed!
echo Executable in: %BUILD_DIR%\Debug

pause
