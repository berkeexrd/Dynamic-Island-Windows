#pragma once
// Frame-by-frame screen recorder for showcase videos:  Aether.exe --record <folder> [--fps 60] [--seconds 300]
// The app then runs on a virtual clock (every frame advances exactly 1/fps seconds) and after every frame the primary
// monitor is grabbed as it is composed by DWM and written as frame_00001.png, frame_00002.png ... No frame can be
// dropped however slow the PC is, so the video is smooth. Turn the frames into a video with tools\make_video.bat.
#include <windows.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cwchar>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <fstream>
#include <filesystem>

namespace aether::core {

struct RecordOptions {
    bool on{false};
    std::wstring dir;
    int fps{60};
    double seconds{300.0};   // safety cap; the scripted tour normally ends the recording
};

inline RecordOptions ParseRecordArgs() {
    RecordOptions o;
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!a) return o;
    for (int i = 1; i < n; ++i) {
        const std::wstring k = a[i];
        if (k == L"--record" && i + 1 < n) { o.on = true; o.dir = a[++i]; }
        else if (k == L"--fps" && i + 1 < n) o.fps = std::max(5, std::min(240, _wtoi(a[++i])));
        else if (k == L"--seconds" && i + 1 < n) o.seconds = std::max(1.0, _wtof(a[++i]));
    }
    LocalFree(a);
    return o;
}

class FrameRecorder {
public:
    bool Start(const RecordOptions& o) {
        m_o = o;
        CreateDirectoryW(o.dir.c_str(), nullptr);
        const HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(mon, &mi)) return false;
        m_x = mi.rcMonitor.left; m_y = mi.rcMonitor.top;
        m_w = mi.rcMonitor.right - mi.rcMonitor.left; m_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_wic)))) return false;
        m_total = static_cast<int>(o.seconds * o.fps + 0.5);
        m_started = true;
        return true;
    }
    [[nodiscard]] bool Active() const { return m_started; }
    [[nodiscard]] bool Done() const { return m_started && (m_force || m_frame >= m_total); }
    void ForceDone() { m_force = true; }
    [[nodiscard]] float Dt() const { return 1.0f / static_cast<float>(m_o.fps); }
    [[nodiscard]] double Time() const { return m_frame / static_cast<double>(m_o.fps); }   // seconds of video so far
    [[nodiscard]] int Frame() const { return m_frame; }

    // Call once per rendered frame.
    // dur = how long this frame stays on screen in the video (1/fps for frame-locked scenes, the real elapsed time for
    // scenes that show other programs). The durations go to frames.ffconcat, which tools\make_video.bat feeds to ffmpeg.
    void Capture(double dur) {
        if (!m_started || Done()) return;
        DwmFlush(); DwmFlush();   // let DWM compose what was just presented
        HDC scr = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(scr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = m_w; bi.bmiHeader.biHeight = -m_h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) {
            HGDIOBJ old = SelectObject(mem, bmp);
            BitBlt(mem, 0, 0, m_w, m_h, scr, m_x, m_y, SRCCOPY | CAPTUREBLT);
            SelectObject(mem, old);
            Save(bits);
        }
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, scr);
        char line[160];
        sprintf_s(line, "file 'frame_%05d.png'\nduration %.5f\n", m_frame + 1, dur);
        m_list += line;
        m_last = m_frame + 1;
        ++m_frame;
    }

private:
    // Encoding PNGs is the slow part: it runs on a worker thread so the app (and the PC) stays responsive while recording.
    struct Job { std::wstring name; std::vector<unsigned char> px; };
    void Save(const void* bgra) {
        Job j;
        wchar_t name[MAX_PATH];
        swprintf_s(name, L"%s\\frame_%05d.png", m_o.dir.c_str(), m_frame + 1);
        j.name = name;
        j.px.resize(static_cast<size_t>(m_w) * m_h * 4);
        memcpy(j.px.data(), bgra, j.px.size());
        for (size_t i = 3; i < j.px.size(); i += 4) j.px[i] = 255;   // BitBlt leaves alpha at 0
        if (!m_worker.joinable()) m_worker = std::thread([this] { Work(); });
        std::unique_lock<std::mutex> l(m_mx);
        m_cv.wait(l, [&] { return m_q.size() < 6; });   // bounded: never more than a few frames of memory
        m_q.push_back(std::move(j));
        m_cv.notify_all();
    }
    void Work() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_mx);
                m_cv.wait(l, [&] { return !m_q.empty() || m_stop; });
                if (m_q.empty()) break;
                j = std::move(m_q.front()); m_q.pop_front();
                m_cv.notify_all();
            }
            if (wic) Encode(wic.Get(), j);
        }
        CoUninitialize();
    }
    void Encode(IWICImagingFactory* wic, const Job& j) const {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICStream> st; ComPtr<IWICBitmapFrameEncode> fr; ComPtr<IPropertyBag2> pb;
        if (FAILED(wic->CreateStream(&st)) || FAILED(st->InitializeFromFilename(j.name.c_str(), GENERIC_WRITE))) return;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache))) return;
        if (FAILED(enc->CreateNewFrame(&fr, &pb)) || FAILED(fr->Initialize(pb.Get()))) return;
        fr->SetSize(static_cast<UINT>(m_w), static_cast<UINT>(m_h));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&fmt);
        fr->WritePixels(static_cast<UINT>(m_h), static_cast<UINT>(m_w) * 4, static_cast<UINT>(j.px.size()), const_cast<unsigned char*>(j.px.data()));
        fr->Commit(); enc->Commit();
    }

public:
    // Wait until every queued frame is on disk (call before the app exits).
    void Finish() {
        if (m_started && m_last > 0 && !m_listWritten) {   // ffconcat needs the last file repeated to honour its duration
            m_listWritten = true;
            char tail[64]; sprintf_s(tail, "file 'frame_%05d.png'\n", m_last);
            std::ofstream f(std::filesystem::path(m_o.dir) / L"frames.ffconcat", std::ios::binary);
            f << m_list << tail;
        }
        { std::lock_guard<std::mutex> l(m_mx); m_stop = true; }
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }
    ~FrameRecorder() { Finish(); }

private:
    std::thread m_worker; std::mutex m_mx; std::condition_variable m_cv; std::deque<Job> m_q; bool m_stop{false};
    RecordOptions m_o;
    Microsoft::WRL::ComPtr<IWICImagingFactory> m_wic;
    int m_x{0}, m_y{0}, m_w{0}, m_h{0}, m_frame{0}, m_total{0};
    bool m_started{false}, m_force{false}, m_listWritten{false};
    std::string m_list; int m_last{0};
};

} // namespace aether::core
