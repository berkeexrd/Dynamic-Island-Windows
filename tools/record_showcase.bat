@echo off
rem Records a showcase of the whole shell (island, widgets, Start menu, desktop menu, Explorer, Recycle Bin, all colour themes).
rem   tools\record_showcase.bat [seconds] [fps]      (frames -> <project>\frames, video -> aether-showcase.mp4)
rem This window closes at once: the real work runs hidden, open windows are minimised and the clean desktop is filmed.
rem Keep your hands off the mouse and keyboard until the "Recording finished" message appears.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0record_showcase.ps1" %*
exit /b
