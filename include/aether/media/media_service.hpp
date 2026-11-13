#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include "aether/core/types.hpp"

namespace aether::media {

enum class PlaybackStatus {
    Closed,
    Opened,
    Changing,
    Stopped,
    Playing,
    Paused
};

struct MediaTrackInfo {
    std::wstring title{L"No Media Playing"};
    std::wstring artist{L"Aether System"};
    std::wstring albumTitle{L""};
    std::wstring sourceApp{L""};
    PlaybackStatus status{PlaybackStatus::Stopped};
    double durationSeconds{0.0};
    double positionSeconds{0.0};
    std::vector<uint8_t> albumArtBytes;
    bool hasMedia{false};
    uint64_t artVersion{0};   // bumps every time albumArtBytes changes
};

struct MediaChangedEvent {
    MediaTrackInfo track;
};

class MediaService {
public:
    static MediaService& Instance() {
        static MediaService instance;
        return instance;
    }

    void Initialize();
    void Shutdown();
    void Update(float dt);

    // Full snapshot (copies the album art bytes - avoid calling this every frame).
    [[nodiscard]] MediaTrackInfo GetCurrentTrack() const;
    // Cheap snapshot without album art, safe to call every frame.
    [[nodiscard]] MediaTrackInfo GetTrackLite() const;
    // Copies album art only if it differs from `knownVersion`. Returns true when `out` was filled.
    bool FetchAlbumArt(uint64_t& knownVersion, std::vector<uint8_t>& out) const;

    // Controls
    void PlayPause();
    void NextTrack();
    void PreviousTrack();
    void Seek(double seconds);
    void SetVolume(float volume01);
    [[nodiscard]] float GetVolume() const;

    [[nodiscard]] bool IsMicrophoneActive() const;
    [[nodiscard]] bool IsCameraActive() const;
    // Peak level (0..1) of whatever is currently rendered to the default speakers - drives the visualizer.
    [[nodiscard]] float GetAudioPeak() const { return m_audioPeak.load(); }

private:
    MediaService() = default;
    ~MediaService() { Shutdown(); }

    void WorkerLoop();
    void MeterLoop();
    void QueryMasterVolume();

    mutable std::mutex m_mutex;
    MediaTrackInfo m_currentTrack;
    std::atomic<float> m_masterVolume{0.5f};
    std::atomic<bool> m_micActive{false};
    std::atomic<bool> m_camActive{false};
    std::atomic<float> m_audioPeak{0.0f};
    std::atomic<bool> m_running{false};
    std::thread m_workerThread;
    std::thread m_meterThread;
    bool m_initialized{false};
};

} // namespace aether::media
