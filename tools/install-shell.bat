@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-shell.ps1" -BinDir "%~dp0.." %*
pause
