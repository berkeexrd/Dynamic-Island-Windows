# Aether kabuk kurulumunu geri alır: explorer.exe tekrar kabuk olur, klasör ilişkilendirmeleri eski haline döner.
$ErrorActionPreference = 'Continue'
$winlogon = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon'
$backup = Join-Path $env:APPDATA 'Aether\shell-backup.json'
$prev = $null
if (Test-Path $backup) { try { $prev = (Get-Content $backup -Raw | ConvertFrom-Json).previousShell } catch {} }
if ($prev) { Set-ItemProperty -Path $winlogon -Name Shell -Value $prev }
else { Remove-ItemProperty -Path $winlogon -Name Shell -ErrorAction SilentlyContinue }
foreach ($k in 'Directory','Folder','Drive') {
    Remove-Item "HKCU:\Software\Classes\$k\shell\open" -Recurse -Force -ErrorAction SilentlyContinue
}
Remove-Item $backup -ErrorAction SilentlyContinue
Write-Host "Geri alındı. Oturumu kapatıp yeniden açın (veya şimdi explorer.exe başlatın)." -ForegroundColor Green
