#include "aether/media/media_service.hpp"
#include "aether/core/log.hpp"
#include "aether/core/events.hpp"

#include <windows.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

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
    m_initialized = true;
    AETHER_LOG_INFO("MediaService initialized with active GSMTC worker thread");
}

void MediaService::Shutdown() {
    if (!m_initialized) return;

    m_running = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
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
                    bool shouldReadArt = (title != lastTitle) || m_currentTrack.albumArtBytes.empty();

                    try {
                        auto props = session.TryGetMediaPropertiesAsync().get();
                        if (props) {
                            title = props.Title().c_str();
                            artist = props.Artist().c_str();
                            album = props.AlbumTitle().c_str();

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
                        if (m_currentTrack.title != title || m_currentTrack.status != status) {
                            trackSwitched = true;
                            changed = true;
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
                            m_currentTrack.albumArtBytes = std::move(artBytes);
                            changed = true;
                        } else if (trackSwitched && artBytes.empty()) {
                            m_currentTrack.albumArtBytes.clear();
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
                        core::EventBus::Instance().Publish(MediaChangedEvent{GetCurrentTrack()});
                    } else if (changed) {
                        core::EventBus::Instance().Publish(MediaChangedEvent{GetCurrentTrack()});
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
                        core::EventBus::Instance().Publish(MediaChangedEvent{GetCurrentTrack()});
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

} // namespace aether::media
