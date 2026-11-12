@echo off
setlocal EnableExtensions

cd /d "%~dp0"

if not exist "%~dp0vc_redist.x64.exe" (
    echo [VC runtime] vc_redist.x64.exe is missing.
    echo Put the official Microsoft x64 installer next to this script.
    pause
    exit /b 1
)

rem Re-launch as administrator when needed.
net session >nul 2>&1
if errorlevel 1 (
    echo [VC runtime] Requesting administrator privileges...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo [VC runtime] Installing Microsoft Visual C++ 2015-2022 x64 runtime...
"%~dp0vc_redist.x64.exe" /install /quiet /norestart
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo [VC runtime] Installation completed successfully.
) else if "%RC%"=="3010" (
    echo [VC runtime] Installation completed. A reboot is required.
) else if "%RC%"=="1638" (
    echo [VC runtime] A newer or equivalent runtime is already installed.
) else (
    echo [VC runtime] Installation failed with exit code %RC%.
    echo You can also run vc_redist.x64.exe manually for detailed diagnostics.
)

echo.
pause
exit /b %RC%
