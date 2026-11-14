# Aether'i Windows kabuğu (explorer.exe yerine) olarak kurar - YALNIZCA bu kullanıcı için, geri alınabilir.
# Kullanım:  .\install-shell.ps1 -BinDir "C:\Aether"      (BinDir: AetherShell.exe'nin olduğu klasör; boşsa bu betiğin klasörü)
param([string]$BinDir = $PSScriptRoot)
$ErrorActionPreference = 'Stop'

$need = 'AetherShell.exe','AetherBar.exe','AetherDesktop.exe','AetherExplorer.exe','AetherBin.exe','Aether.exe'
$missing = $need | Where-Object { -not (Test-Path (Join-Path $BinDir $_)) }
if ($missing) {
    Write-Host "Şu dosyalar '$BinDir' içinde yok: $($missing -join ', ')" -ForegroundColor Red
    Write-Host "Önce hepsini derleyip aynı klasöre koyun (veya -BinDir ile doğru klasörü verin)."
    exit 1
}
$BinDir = (Resolve-Path $BinDir).Path

# 1) yedek: önceki kabuk değeri
$dir = Join-Path $env:APPDATA 'Aether'
New-Item -ItemType Directory -Force $dir | Out-Null
$backup = Join-Path $dir 'shell-backup.json'
$winlogon = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Winlogon'
if (-not (Test-Path $backup)) {
    $prev = $null
    if (Test-Path $winlogon) { $prev = (Get-ItemProperty $winlogon -ErrorAction SilentlyContinue).Shell }
    @{ previousShell = $prev; binDir = $BinDir; installedAt = (Get-Date).ToString('o') } | ConvertTo-Json | Set-Content $backup -Encoding UTF8
}

# 2) oturum açılınca explorer.exe yerine AetherShell.exe çalışsın (yalnızca bu kullanıcı)
if (-not (Test-Path $winlogon)) { New-Item $winlogon -Force | Out-Null }
Set-ItemProperty -Path $winlogon -Name Shell -Value ('"' + $BinDir + '\AetherShell.exe"')

# 3) klasörler / sürücüler Aether Explorer'da açılsın
$cmd = '"' + $BinDir + '\AetherExplorer.exe" "%1"'
foreach ($k in 'Directory','Folder','Drive') {
    $key = "HKCU:\Software\Classes\$k\shell\open\command"
    New-Item $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name '(default)' -Value $cmd
    New-ItemProperty -Path $key -Name 'DelegateExecute' -Value '' -PropertyType String -Force | Out-Null
}

Write-Host ""
Write-Host "Kuruldu. Oturumu kapatıp yeniden açın." -ForegroundColor Green
Write-Host "Acil durum: oturum açarken SHIFT tuşunu basılı tutun -> normal Windows masaüstü açılır."
Write-Host "Geri almak için: uninstall-shell.ps1"
