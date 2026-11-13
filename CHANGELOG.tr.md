# Değişiklik Günlüğü

## [1.2.0] - 2026-11-13

### Eklendi - Aether Bar (yeni, ayrı `AetherBar.exe`)
- Island ile aynı Direct2D + DirectComposition altyapısı üstünde **görev çubuğu değiştirici**.
- Ortada yüzen squircle dock + ayrı sistem kapsülü (pil, saat, tarih).
- Pencereler uygulamaya göre gruplanır; ikonlar pencerelerin kendisinden alınır.
- **Sabitlenmiş uygulamalar** (`%APPDATA%\Aether\bar_pins.txt`), sağ tık menüsünden sabitle/kaldır.
- Tıkla: aç / küçült. Çoklu pencerede tekrar tıkla: pencereler arasında geç. Orta tuş: yeni örnek.
- Hover büyütme animasyonu, aktif uygulama göstergesi, pencere başlıklı tooltip, spring tabanlı yeniden dizilim.
- Başlat butonu gerçek Windows Başlat menüsünü açar.
- Gerçek **AppBar** olarak kayıt olur: büyütülmüş pencereler çubuğun üstünde durur, tam ekran uygulamada gizlenir.
- Windows görev çubuğunu gizler, Explorer yeniden başlarsa tekrar gizler.
- **Güvenlik:** çıkışta, çökmede, oturum kapatmada ve `AetherBar.exe --restore` ile eski görev çubuğu geri gelir.

### Bilinen sınırlamalar
- Sistem tepsisi (bildirim alanı) henüz yok, görev çubuğuyla birlikte gizlenir.
- Sadece ana monitör.

## [1.1.0] - 2026-11-13

### Değişti - Dynamic Island görünümü (IslandWindow baştan yazıldı)
- Gerçek donanım çentiği gibi saf siyah gövde; neon / buzlu cam görünümü kaldırıldı.
- Gerçek **sürekli eğrilikli squircle** köşeler; kapsülden karta temiz geçiş.
- Yumuşak gölge ve ince kenar parlaması.
- Geçişlerde içerik **blur + ölçekle** belirir, şeklin dışına taşmaz.
- Yeni medya düzeni: kapak, başlık/sanatçı, kalan süreli scrubber, büyük kontroller.
- Uzun başlıklar kenarları solarak kayar (marquee).
- Oynat/duraklat ikonları spring ile geçiş yapar; butonlar basınca küçülür, bırakınca çalışır.
- Activity-ring tarzı sistem görünümü (CPU / RAM / GPU), sabit "GPU 4060" yazısı kaldırıldı.
- Seviyeye göre ikonlu ses HUD'ı, ikon kutulu iki satırlı bildirim.
- Segoe UI Variable, Windows 10'da otomatik Segoe UI yedeği.

### Eklendi
- **Albüm rengi**: kapaktan baskın canlı renk çıkarılır; görselleştirici, parıltı ve parlamayı boyar.
- **Peek banner**: yeni şarkı başlayınca başlığı kısa süre gösterir (iOS gibi).
- **Gerçek ses görselleştirici**: çubuklar varsayılan ses cihazının gerçek seviyesini izler.
- **Gizlilik göstergeleri**: mikrofon için turuncu, kamera için yeşil nokta.
- Sürüklenebilir scrubber ve ses slider'ı; island üstünde fare tekerleği ile ses; kontrollerde el imleci.
- `reducedMotion`, `timeScale`, `showClockIdle`, `showAudioVisualizer` ayarlarına uyar.
- DPI'ya duyarlı pencere (%150 / %200'de kırpılma yok).
- Eksik olan `CMakeLists.txt` (ImGui yoksa otomatik indirilir).

### Düzeltildi
- Albüm kapağı başlık okunmadan karşılaştırıldığı için her 350 ms'de yeniden okunuyordu.
- `GetCurrentTrack()` kapak resmini her çağrıda kopyalıyordu (saniyede yüzlerce kez).
- Medya olay işleyicisi worker thread'den UI'a dokunuyordu (yarış durumu).
- Duraklat/devam et yapınca kapak siliniyordu.
- Mikrofon durumu hiç doldurulmuyordu.
- Ambient animasyon için tam hızda çizim yapılıyordu; artık ~60 fps, hiçbir şey değişmiyorsa tam uyku.

### Kaldırıldı
- Sahte blur için her karede masaüstü ekran görüntüsü (`BitBlt`, ~12 fps).
