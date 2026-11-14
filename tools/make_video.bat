@echo off
rem Turns the frames recorded by "Aether.exe --record" into an MP4 (needs ffmpeg on PATH: https://ffmpeg.org).
rem   tools\make_video.bat [frames-folder] [fps] [output]
setlocal
set "DIR=%~1"
if "%DIR%"=="" set "DIR=frames"
set "FPS=%~2"
if "%FPS%"=="" set "FPS=60"
set "OUT=%~3"
if "%OUT%"=="" set "OUT=aether-showcase.mp4"
where ffmpeg >nul 2>nul || (echo ffmpeg not found. Install it from https://ffmpeg.org and add it to PATH. & exit /b 1)
if exist "%DIR%\frames.ffconcat" (
    ffmpeg -y -f concat -safe 0 -i "%DIR%\frames.ffconcat" -vf "fps=%FPS%" -c:v libx264 -preset slow -crf 14 -pix_fmt yuv420p "%OUT%"
) else (
    ffmpeg -y -framerate %FPS% -i "%DIR%\frame_%%05d.png" -c:v libx264 -preset slow -crf 14 -pix_fmt yuv420p "%OUT%"
)
echo Done: %OUT%
