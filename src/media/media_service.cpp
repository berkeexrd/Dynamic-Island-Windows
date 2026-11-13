#include "aether/media/media_service.hpp"
#include "aether/core/log.hpp"
#include "aether/core/events.hpp"

#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <chrono>
#include <algorithm>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

namespace winrtMedia = winrt::Windows::Media::Control;

namespace aether::media {

static winrtMedia::GlobalSystemMediaTransportControlsSessionManager s_sessionManager{nullptr};

void MediaService::Initialize() {
    if (m_initialized) return;

    QueryMasterVolume();
    m_running = true;
    m_workerThread = std::thread(&MediaService::WorkerLoop, this);
    m_meterThread = std::thread(&MediaService::MeterLoop, this);
    m_initialized = true;
    AETHER_LOG_INFO("MediaService initialized with active GSMTC worker thread");
}

void MediaService::Shutdown() {
    if (!m_initialized) return;

    m_running = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    if (m_meterThread.joinable()) {
        m_meterThread.join();
    }

    s_sessionManager = nullptr;
    m_initialized = false;
}

static winrtMedia::GlobalSystemMediaTransportControlsSession FindBestSession(
    winrtMedia::GlobalSystemMediaTransportControlsSessionManager const& mgr) {
    if (!mgr) return nullptr;

    try {
        auto current = mgr.GetCurrentSession();
        if (current) {
            auto pb = current.GetPlaybackInfo();
            if (pb && pb.PlaybackStatus() == winrtMedia::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                return current;
            }
        }

        // If CurrentSession is not playing or null, scan all active sessions
        auto sessions = mgr.GetSessions();
        winrtMedia::GlobalSystemMediaTransportControlsSession fallbackSession{nullptr};

        for (auto const& s : sessions) {
            if (!s) continue;
            auto pb = s.GetPlaybackInfo();
            if (pb) {
                if (pb.PlaybackStatus() == winrtMedia::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                    return s; // Priority: actively playing session (e.g. Opera GX, Spotify, Chrome)
                }
                if (!fallbackSession) {
                    fallbackSession = s;
                }
            }
        }

        return current ? current : fallbackSession;
    } catch (...) {
        return nullptr;
    }
}

void MediaService::WorkerLoop() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {}

    std::wstring lastTitle = L"";
    PlaybackStatus lastStatus = PlaybackStatus::Stopped;
    // Album art is fetched a few times right after a track change (players often publish the
    // thumbnail a moment after the title) and then left alone - it used to be re-read every poll.
    std::wstring artTitle = L"";
    int artTries = 0;
    bool artDone = false;

    while (m_running) {
        try {
            if (!s_sessionManager) {
                auto op = winrtMedia::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
                s_sessionManager = op.get();
            }

            if (s_sessionManager) {
                auto session = FindBestSession(s_sessionManager);
                if (session) {
                    std::wstring title, artist, album, appName;
                    PlaybackStatus status = PlaybackStatus::Stopped;
                    double posSec = 0.0;
                    double durSec = 0.0;

                    try {
                        appName = session.SourceAppUserModelId().c_str();
                    } catch (...) {}

                    std::vector<uint8_t> artBytes;
                    bool shouldReadArt = false;

                    try {
                        auto props = session.TryGetMediaPropertiesAsync().get();
                        if (props) {
                            title = props.Title().c_str();
                            artist = props.Artist().c_str();
                            album = props.AlbumTitle().c_str();

                            if (title != artTitle) {
                                artTitle = title;
                                artTries = 0;
                                artDone = false;
                            }
                            if (!artDone && artTries < 6 && !title.empty()) {
                                shouldReadArt = true;
                                ++artTries;
                            }

                            if (shouldReadArt) {
                                auto thumbRef = props.Thumbnail();
                                if (thumbRef) {
                                    auto stream = thumbRef.OpenReadAsync().get();
                                    if (stream) {
                                        uint64_t size = stream.Size();
                                        if (size > 0 && size < 12 * 1024 * 1024) {
                                            winrt::Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
                                            reader.LoadAsync(static_cast<uint32_t>(size)).get();
                                            artBytes.resize(static_cast<size_t>(size));
                                            reader.ReadBytes(winrt::array_view<uint8_t>(artBytes));
                                        }
                                    }
                                }
                            }
                        }
                    } catch (...) {}

                    try {
                        auto pbInfo = session.GetPlaybackInfo();
                        if (pbInfo) {
                            auto s = pbInfo.PlaybackStatus();
                            if (s == winrtMedia::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                                status = PlaybackStatus::Playing;
                            } else if (s == winrtMedia::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused) {
                                status = PlaybackStatus::Paused;
                            }
                        }
                    } catch (...) {}

                    try {
                        auto timeline = session.GetTimelineProperties();
                        if (timeline) {
                            posSec = static_cast<double>(timeline.Position().count()) / 10000000.0;
                            durSec = static_cast<double>(timeline.EndTime().count()) / 10000000.0;
                        }
                    } catch (...) {}

                    bool changed = false;
                    bool trackSwitched = false;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        const bool titleChanged = (m_currentTrack.title != title);
                        if (titleChanged || m_currentTrack.status != status) {
                            trackSwitched = true;
                            changed = true;
                        }
                        if (titleChanged && !m_currentTrack.albumArtBytes.empty()) {
                            // Never keep showing the previous song's cover.
                            m_currentTrack.albumArtBytes.clear();
                            ++m_currentTrack.artVersion;
                        }
                        if (m_currentTrack.durationSeconds != durSec) {
                            changed = true;
                        }

                        m_currentTrack.title = title;
                        m_currentTrack.artist = artist;
                        m_currentTrack.albumTitle = album;
                        m_currentTrack.sourceApp = appName;
                        m_currentTrack.status = status;
                        m_currentTrack.durationSeconds = durSec;
                        if (shouldReadArt && !artBytes.empty()) {
                            if (artBytes != m_currentTrack.albumArtBytes) {
                                m_currentTrack.albumArtBytes = std::move(artBytes);
                                ++m_currentTrack.artVersion;
                                changed = true;
                            }
                            if (artTries >= 2) artDone = true;
                        }
                        // Resync position if significant jump or new track
                        if (trackSwitched) {
                            m_currentTrack.positionSeconds = (posSec > 0.1) ? posSec : 0.0;
                        } else if (posSec > 0.5) {
                            if (std::abs(m_currentTrack.positionSeconds - posSec) > 3.5) {
                                m_currentTrack.positionSeconds = posSec;
                            }
                        }
                        m_currentTrack.hasMedia = !title.empty();
                    }

                    if (trackSwitched) {
                        lastTitle = title;
                        lastStatus = status;
                        AETHER_LOG_INFO("GSMTC Track Active: '" << winrt::to_string(title) 
                            << "' by '" << winrt::to_string(artist) 
                            << "' (" << winrt::to_string(appName) << ") | Duration: " 
                            << static_cast<int>(durSec) << "s");
                        core::EventBus::Instance().Publish(MediaChangedEvent{GetTrackLite()});
                    } else if (changed) {
                        core::EventBus::Instance().Publish(MediaChangedEvent{GetTrackLite()});
                    }
                } else {
                    bool hadMedia = false;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        hadMedia = m_currentTrack.hasMedia;
                        m_currentTrack.hasMedia = false;
                        m_currentTrack.status = PlaybackStatus::Stopped;
                    }
                    if (hadMedia) {
                        lastTitle = L"";
                        lastStatus = PlaybackStatus::Stopped;
                        core::EventBus::Instance().Publish(MediaChangedEvent{GetTrackLite()});
                    }
                }
            }
        } catch (const winrt::hresult_error& err) {
            AETHER_LOG_WARN("GSMTC Worker Exception: " << winrt::to_string(err.message()));
        } catch (...) {}

        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }

    try {
        winrt::uninit_apartment();
    } catch (...) {}
}

void MediaService::Update(float dt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_currentTrack.hasMedia && m_currentTrack.status == PlaybackStatus::Playing) {
        m_currentTrack.positionSeconds += dt;
        if (m_currentTrack.durationSeconds > 0.0 && m_currentTrack.positionSeconds > m_currentTrack.durationSeconds) {
            m_currentTrack.positionSeconds = m_currentTrack.durationSeconds;
        }
    }
}

void MediaService::QueryMasterVolume() {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<IMMDevice> defaultDevice;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice);
        if (SUCCEEDED(hr)) {
            Microsoft::WRL::ComPtr<IAudioEndpointVolume> endpoint;
            hr = defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&endpoint);
            if (SUCCEEDED(hr)) {
                float vol = 0.0f;
                endpoint->GetMasterVolumeLevelScalar(&vol);
                m_masterVolume = vol;
            }
        }
    }
}

MediaTrackInfo MediaService::GetCurrentTrack() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentTrack;
}

MediaTrackInfo MediaService::GetTrackLite() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    MediaTrackInfo t;
    t.title = m_currentTrack.title;
    t.artist = m_currentTrack.artist;
    t.albumTitle = m_currentTrack.albumTitle;
    t.sourceApp = m_currentTrack.sourceApp;
    t.status = m_currentTrack.status;
    t.durationSeconds = m_currentTrack.durationSeconds;
    t.positionSeconds = m_currentTrack.positionSeconds;
    t.hasMedia = m_currentTrack.hasMedia;
    t.artVersion = m_currentTrack.artVersion;
    return t;
}

bool MediaService::FetchAlbumArt(uint64_t& knownVersion, std::vector<uint8_t>& out) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_currentTrack.artVersion == knownVersion) return false;
    knownVersion = m_currentTrack.artVersion;
    out = m_currentTrack.albumArtBytes;
    return true;
}

void MediaService::PlayPause() {
    try {
        if (s_sessionManager) {
            auto session = FindBestSession(s_sessionManager);
            if (session) {
                session.TryTogglePlayPauseAsync();
                return;
            }
        }
    } catch (...) {}
}

void MediaService::NextTrack() {
    try {
        if (s_sessionManager) {
            auto session = FindBestSession(s_sessionManager);
            if (session) {
                session.TrySkipNextAsync();
                return;
            }
        }
    } catch (...) {}
}

void MediaService::PreviousTrack() {
    try {
        if (s_sessionManager) {
            auto session = FindBestSession(s_sessionManager);
            if (session) {
                session.TrySkipPreviousAsync();
                return;
            }
        }
    } catch (...) {}
}

void MediaService::Seek(double seconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_currentTrack.hasMedia) return;
    if (m_currentTrack.durationSeconds > 0.0) {
        seconds = std::clamp(seconds, 0.0, m_currentTrack.durationSeconds);
    }
    m_currentTrack.positionSeconds = seconds;

    try {
        if (s_sessionManager) {
            auto session = FindBestSession(s_sessionManager);
            if (session) {
                int64_t ticks = static_cast<int64_t>(seconds * 10000000.0);
                session.TryChangePlaybackPositionAsync(ticks);
            }
        }
    } catch (...) {}
}

void MediaService::SetVolume(float volume01) {
    m_masterVolume = std::clamp(volume01, 0.0f, 1.0f);
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        Microsoft::WRL::ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice))) {
            Microsoft::WRL::ComPtr<IAudioEndpointVolume> endpoint;
            if (SUCCEEDED(defaultDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&endpoint))) {
                endpoint->SetMasterVolumeLevelScalar(m_masterVolume, nullptr);
            }
        }
    }
}

float MediaService::GetVolume() const {
    return m_masterVolume;
}

bool MediaService::IsMicrophoneActive() const {
    return m_micActive;
}

bool MediaService::IsCameraActive() const {
    return m_camActive;
}

// Windows records, per app, when it last started/stopped using a capability. An app that started
// and has not stopped yet (Stop == 0 or Start > Stop) is using the mic/camera right now.
static bool ReadLastUsedActive(HKEY key) {
    ULONGLONG start = 0, stop = 0;
    DWORD size = sizeof(start);
    if (RegGetValueW(key, nullptr, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &size) != ERROR_SUCCESS) return false;
    size = sizeof(stop);
    if (RegGetValueW(key, nullptr, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &size) != ERROR_SUCCESS) return false;
    return start != 0 && start > stop;
}

static bool CapabilityInUseUnder(const std::wstring& path) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &root) != ERROR_SUCCESS) return false;
    bool active = false;
    wchar_t name[512];
    for (DWORD i = 0; !active; ++i) {
        DWORD len = ARRAYSIZE(name);
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) == ERROR_SUCCESS) {
            active = ReadLastUsedActive(sub);
            RegCloseKey(sub);
        }
    }
    RegCloseKey(root);
    return active;
}

static bool IsCapabilityInUse(const wchar_t* capability) {
    std::wstring base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    base += capability;
    return CapabilityInUseUnder(base) || CapabilityInUseUnder(base + L"\\NonPackaged");
}

void MediaService::MeterLoop() {
    const bool comOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    Microsoft::WRL::ComPtr<IAudioMeterInformation> meter;
    auto lastAcquire = std::chrono::steady_clock::time_point{};
    auto lastPrivacy = std::chrono::steady_clock::time_point{};

    while (m_running) {
        const auto now = std::chrono::steady_clock::now();

        // (Re)bind to the default speakers every few seconds so device switches are picked up.
        if (!meter || now - lastAcquire > std::chrono::seconds(3)) {
            lastAcquire = now;
            meter.Reset();
            Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
                Microsoft::WRL::ComPtr<IMMDevice> dev;
                if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
                    dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
                }
            }
        }

        float peak = 0.0f;
        if (meter && SUCCEEDED(meter->GetPeakValue(&peak))) {
            m_audioPeak = std::clamp(peak, 0.0f, 1.0f);
        } else {
            meter.Reset();
            m_audioPeak = 0.0f;
        }

        if (now - lastPrivacy > std::chrono::milliseconds(1000)) {
            lastPrivacy = now;
            m_micActive = IsCapabilityInUse(L"microphone");
            m_camActive = IsCapabilityInUse(L"webcam");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    meter.Reset();
    if (comOk) CoUninitialize();
}

} // namespace aether::media
