@echo off
setlocal EnableExtensions

set "ROOT=%~dp0.."
cd /d "%ROOT%"

set "BUILD_DIR=%ROOT%\build\sdl3"
set "REL_DIR=%ROOT%\release"
set "VC_REDIST_CACHE=%ROOT%\build_scripts\redist\vc_redist.x64.exe"
set "RELEASE_ASSETS=%ROOT%\build_scripts\release_assets"

call "d:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64
if errorlevel 1 (
    echo [release] VsDevCmd.bat not found or failed.
    exit /b 1
)

echo [release] Configuring and building Release...
cmake -S "%ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -DUICORNERSTONE_BACKEND=sdl3
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

if exist "%REL_DIR%" rmdir /s /q "%REL_DIR%"
mkdir "%REL_DIR%"
mkdir "%REL_DIR%\gen"
mkdir "%REL_DIR%\schema"
mkdir "%REL_DIR%\sdk"

echo [release] Copying runtime files...
copy /Y "%BUILD_DIR%\Release\CornerstoneDesigner.exe" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\CornerstoneLauncher.exe" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\UICornerstone.dll" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\UIBackend_sdl3.dll" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\SDL3.dll" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\SDL3_image.dll" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\SDL3_ttf.dll" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\main_layout.json" "%REL_DIR%\" >nul || goto :fail
copy /Y "%BUILD_DIR%\Release\designer_theme.json" "%REL_DIR%\" >nul || goto :fail

echo [release] Copying assets and runtime data...
cmake -E copy_directory "%BUILD_DIR%\Release\assets" "%REL_DIR%\assets"
if errorlevel 1 goto :fail
cmake -E copy_directory "%BUILD_DIR%\Release\gen\icons" "%REL_DIR%\gen\icons"
if errorlevel 1 goto :fail
cmake -E copy_directory "%BUILD_DIR%\Release\schema" "%REL_DIR%\schema"
if errorlevel 1 goto :fail
cmake -E copy_directory "%BUILD_DIR%\Release\sdk" "%REL_DIR%\sdk"
if errorlevel 1 goto :fail
cmake -E copy_directory "%BUILD_DIR%\Release\themes" "%REL_DIR%\themes"
if errorlevel 1 goto :fail

echo [release] Copying documentation and license...
copy /Y "%ROOT%\README.md" "%REL_DIR%\" >nul || goto :fail
copy /Y "%ROOT%\LICENSE" "%REL_DIR%\" >nul || goto :fail
cmake -E copy_directory "%ROOT%\docs" "%REL_DIR%\docs"
if errorlevel 1 goto :fail

echo [release] Adding Visual C++ runtime installer...
if not exist "%VC_REDIST_CACHE%" (
    if not exist "%ROOT%\build_scripts\redist" mkdir "%ROOT%\build_scripts\redist"
    echo [release] Downloading official vc_redist.x64.exe...
    curl.exe -L --fail --retry 3 -o "%VC_REDIST_CACHE%" "https://aka.ms/vs/17/release/vc_redist.x64.exe"
    if errorlevel 1 (
        echo [release] Download failed. Place vc_redist.x64.exe in build_scripts\redist and retry.
        goto :fail
    )
)
copy /Y "%VC_REDIST_CACHE%" "%REL_DIR%\vc_redist.x64.exe" >nul || goto :fail
copy /Y "%RELEASE_ASSETS%\install_vc_runtime.bat" "%REL_DIR%\install_vc_runtime.bat" >nul || goto :fail
copy /Y "%RELEASE_ASSETS%\VC_Runtime_README_zh-CN.txt" "%REL_DIR%\VC_Runtime_README_zh-CN.txt" >nul || goto :fail

echo.
echo [release] Package prepared at:
echo   %REL_DIR%
echo.
echo [release] Included Visual C++ runtime:
echo   vc_redist.x64.exe
echo   install_vc_runtime.bat
echo   VC_Runtime_README_zh-CN.txt
echo.
echo [release] Notes:
echo   - Do not copy developer settings, logs or Temp artifacts.
echo   - Target machines need the Microsoft Visual C++ 2015-2022 x64 runtime.
echo   - Test the complete package on a clean Windows machine before publishing.
exit /b 0

:fail
echo [release] Copy failed. Check the paths above.
exit /b 1
