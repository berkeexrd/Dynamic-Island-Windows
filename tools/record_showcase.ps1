# Records the showcase tour (see tools\record_showcase.bat). Runs in a hidden window, so nothing of it shows in the video.
param([int]$Seconds = 300, [int]$Fps = 60)
$ErrorActionPreference = 'Stop'
# AetherBar runs as administrator and Windows ignores keyboard/mouse input that a normal program sends to it (Start menu, menus):
# ask for administrator rights once (UAC prompt), then continue hidden.
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -Seconds $Seconds -Fps $Fps"
    exit
}
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$ui = New-Object -ComObject WScript.Shell
$exe = Join-Path $root 'build\Release\Aether.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $root 'Aether.exe' }
if (-not (Test-Path $exe)) { $ui.Popup('Aether.exe not found. Build first with build_setup.bat.', 0, 'Aether', 16) | Out-Null; exit 1 }
if (Test-Path 'frames') { Remove-Item 'frames' -Recurse -Force }

# every Aether process (bar, desktop, ...) follows %APPDATA%\Aether\lang.txt: English while recording, restored afterwards
$lf = Join-Path $env:APPDATA 'Aether\lang.txt'
New-Item -ItemType Directory -Force (Split-Path $lf) | Out-Null
$old = 'auto'
if (Test-Path $lf) { $t = (Get-Content $lf -Raw); if ($t) { $old = $t.Trim() } }
try {
    Set-Content -Path $lf -Value 'en' -NoNewline
    Start-Sleep -Seconds 4
    # the recorder minimises every open window itself and then films the clean desktop
    Start-Process -FilePath $exe -ArgumentList '--record', 'frames', '--fps', $Fps, '--seconds', $Seconds -WorkingDirectory $root -Wait
} finally {
    Set-Content -Path $lf -Value $old -NoNewline
}

if (-not (Test-Path 'frames\frame_00001.png')) { $ui.Popup('Aether recording: no frames were written.', 0, 'Aether', 16) | Out-Null; exit 1 }
if (Get-Command ffmpeg -ErrorAction SilentlyContinue) { & (Join-Path $PSScriptRoot 'make_video.bat') 'frames' $Fps 'aether-showcase.mp4' | Out-Null }
$ui.Popup("Recording finished.`n`nFrames: $root\frames`n(video: $root\aether-showcase.mp4 if ffmpeg is installed)", 15, 'Aether', 64) | Out-Null
