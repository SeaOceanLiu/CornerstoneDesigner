@echo off
rem 清理本轮手工/自动验证残留的 Launcher 与 Designer 进程。
taskkill /F /IM CornerstoneLauncher.exe >nul 2>&1
taskkill /F /IM CornerstoneDesigner.exe >nul 2>&1
exit /b 0
