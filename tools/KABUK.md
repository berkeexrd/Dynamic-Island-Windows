# Aether'i tam kabuk olarak kullanmak

Aether, `explorer.exe` yerine oturumun kabuğu olarak çalışabilir. Kurulum yalnızca **bu kullanıcıyı** etkiler ve geri alınabilir.

> **Kolay yol:** `AetherSetup.exe` bu adımların hepsini sihirbazla yapar ("Windows kabuğu yerine kullan" seçeneği) ve kaldırıcıyı da kurar. Aşağısı installer'sız elle kurulumdur.

## Gerekenler
Aynı klasörde (ör. `C:\Aether`): `AetherShell.exe`, `AetherIntro.exe`, `AetherBar.exe`, `AetherDesktop.exe`, `AetherExplorer.exe`, `AetherBin.exe`, `Aether.exe`.

## Kurulum
1. Önce **normal oturumda** deneyin: `AetherShell.exe`'yi çalıştırın (Explorer çalışırken yalnızca Alt+Tab, Win+Tab, Win+R, Win+M, Win+I devreye girer).
2. Her şey tamamsa `tools\install-shell.bat` (çift tıklayın), ardından oturumu kapatıp açın.

## Geri alma / acil durum
- Oturum açarken **Shift** basılı tutun: o oturumda normal Windows masaüstü açılır.
- Siyah ekranda: `Ctrl+Shift+Esc` (Görev Yöneticisi) -> Dosya -> Yeni görev -> `explorer.exe` (veya `powershell` yazıp `tools\uninstall-shell.ps1` çalıştırın).
- Kalıcı geri alma: `tools\uninstall-shell.bat`.
- `%APPDATA%\Aether\use-explorer` adında boş bir dosya oluşturmak da Windows kabuğunu zorlar.

## AetherShell ne yapar?
- AetherBar, AetherDesktop ve Aether'i başlatır; çökerlerse yeniden başlatır (çubuk sürekli çökerse Windows kabuğuna döner).
- Oturum açılışında Explorer'ın yaptığı başlangıç işlerini yapar: `Run`, `RunOnce`, Başlangıç klasörleri (StartupApproved ayarına uyar).
- Alt+Tab, Win+Tab (kalıcı görev görünümü), Win+R (Çalıştır), Win+M / Win+Shift+M, Win+I, Win+D, Win+E.
- Klasör açan her şey AetherExplorer'a gider; Geri Dönüşüm Kutusu AetherBin'de açılır.
