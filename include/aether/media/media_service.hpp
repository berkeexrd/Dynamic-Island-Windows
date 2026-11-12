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

    [[nodiscard]] MediaTrackInfo GetCurrentTrack() const;

    // Controls
    void PlayPause();
    void NextTrack();
    void PreviousTrack();
    void Seek(double seconds);
    void SetVolume(float volume01);
    [[nodiscard]] float GetVolume() const;

    [[nodiscard]] bool IsMicrophoneActive() const;

private:
    MediaService() = default;
    ~MediaService() { Shutdown(); }

    void WorkerLoop();
    void QueryMasterVolume();

    mutable std::mutex m_mutex;
    MediaTrackInfo m_currentTrack;
    std::atomic<float> m_masterVolume{0.5f};
    std::atomic<bool> m_micActive{false};
    std::atomic<bool> m_running{false};
    std::thread m_workerThread;
    bool m_initialized{false};
};

} // namespace aether::media
