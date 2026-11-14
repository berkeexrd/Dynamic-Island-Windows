@echo off
rem Builds every Aether program in Release and packs them into the single-file installer.
rem Result: dist\AetherSetup.exe
cd /d "%~dp0"
taskkill /im AetherShell.exe /f >nul 2>&1
taskkill /im AetherDesktop.exe /f >nul 2>&1
taskkill /im AetherBar.exe /f >nul 2>&1
taskkill /im AetherExplorer.exe /f >nul 2>&1
taskkill /im Aether.exe /f >nul 2>&1
if not exist build cmake -S . -B build
cmake --build build --config Release --target AetherSetup
if errorlevel 1 (echo BUILD FAILED & pause & exit /b 1)
if not exist dist mkdir dist
copy /y build\Release\AetherSetup.exe dist\AetherSetup.exe >nul
echo.
echo Done: %~dp0dist\AetherSetup.exe
pause
