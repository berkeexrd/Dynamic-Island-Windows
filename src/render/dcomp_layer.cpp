#include "aether/render/dcomp_layer.hpp"
#include "aether/core/log.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")

namespace aether::render {

DCompLayer::DCompLayer() = default;

DCompLayer::~DCompLayer() {
    Cleanup();
}

void DCompLayer::Cleanup() {
    if (m_dcompTarget) {
        m_dcompTarget->SetRoot(nullptr);
    }
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();

    m_d2dTargetBitmap.Reset();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_dwriteFactory.Reset();

    m_swapChain.Reset();
    m_d3d11Context.Reset();
    m_d3d11Device.Reset();
    m_hwnd = nullptr;
}

bool DCompLayer::CreateDeviceResources() {
    // 1. Create D3D11 Device with BGRA support for D2D interop
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1
    };

    D3D_FEATURE_LEVEL actualLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3d11Device,
        &actualLevel,
        &m_d3d11Context
    );

    if (FAILED(hr)) {
        // Fallback without debug flag if debug layer unavailable
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &m_d3d11Device,
            &actualLevel,
            &m_d3d11Context
        );
        if (FAILED(hr)) {
            AETHER_LOG_ERROR("Failed to create D3D11 device: 0x" << std::hex << hr);
            return false;
        }
    }

    // 2. Query IDXGIDevice
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_d3d11Device.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    // 3. Create Direct2D Factory & Device
    D2D1_FACTORY_OPTIONS d2dOptions = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2dOptions, (void**)&m_d2dFactory);
    if (FAILED(hr)) {
        AETHER_LOG_ERROR("Failed to create D2D1 factory: 0x" << std::hex << hr);
        return false;
    }

    hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
    if (FAILED(hr)) return false;

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
    if (FAILED(hr)) return false;

    // 4. Create DirectWrite Factory
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&m_dwriteFactory);
    if (FAILED(hr)) return false;

    // 5. Create DirectComposition Device
    hr = DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice), (void**)&m_dcompDevice);
    if (FAILED(hr)) {
        AETHER_LOG_ERROR("Failed to create DirectComposition device: 0x" << std::hex << hr);
        return false;
    }

    return true;
}

bool DCompLayer::CreateSwapChain(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    m_width = width;
    m_height = height;

    ComPtr<IDXGIDevice> dxgiDevice;
    m_d3d11Device.As(&dxgiDevice);

    ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(&dxgiAdapter);

    ComPtr<IDXGIFactory2> dxgiFactory;
    dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

    DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
    swapDesc.Width = width;
    swapDesc.Height = height;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.Stereo = FALSE;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SampleDesc.Quality = 0;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // Essential for per-pixel transparency

    HRESULT hr = dxgiFactory->CreateSwapChainForComposition(
        m_d3d11Device.Get(),
        &swapDesc,
        nullptr,
        &m_swapChain
    );

    if (FAILED(hr)) {
        AETHER_LOG_ERROR("Failed to create composition swap chain: 0x" << std::hex << hr);
        return false;
    }

    // Set swap chain as target for DComp Visual
    if (!m_dcompTarget) {
        hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
        if (FAILED(hr)) return false;

        hr = m_dcompDevice->CreateVisual(&m_dcompVisual);
        if (FAILED(hr)) return false;

        m_dcompTarget->SetRoot(m_dcompVisual.Get());
    }

    hr = m_dcompVisual->SetContent(m_swapChain.Get());
    if (FAILED(hr)) return false;

    m_dcompDevice->Commit();

    // Bind D2D Bitmap to SwapChain backbuffer
    ComPtr<IDXGISurface> backBuffer;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f
    );

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(backBuffer.Get(), &bitmapProperties, &m_d2dTargetBitmap);
    if (FAILED(hr)) return false;

    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
    m_d2dContext->SetDpi(96.0f, 96.0f);
    // ClearType cannot be used on a premultiplied-alpha surface; grayscale keeps text crisp and artifact free.
    m_d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

bool DCompLayer::Initialize(HWND hwnd, uint32_t width, uint32_t height) {
    m_hwnd = hwnd;
    if (!CreateDeviceResources()) return false;
    if (!CreateSwapChain(width, height)) return false;
    AETHER_LOG_INFO("DCompLayer initialized successfully for HWND " << hwnd << " (" << width << "x" << height << ")");
    return true;
}

void DCompLayer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || (width == m_width && height == m_height)) return;

    m_d2dContext->SetTarget(nullptr);
    m_d2dTargetBitmap.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(
        2,
        width,
        height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        0
    );

    if (SUCCEEDED(hr)) {
        m_width = width;
        m_height = height;

        ComPtr<IDXGISurface> backBuffer;
        m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));

        D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f
        );

        m_d2dContext->CreateBitmapFromDxgiSurface(backBuffer.Get(), &bitmapProperties, &m_d2dTargetBitmap);
        m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
        m_dcompDevice->Commit();
    }
}

ID2D1DeviceContext* DCompLayer::BeginDraw() {
    if (!m_d2dContext) return nullptr;
    m_d2dContext->BeginDraw();
    m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
    m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0.0f)); // Transparent clear
    return m_d2dContext.Get();
}

void DCompLayer::EndDraw() {
    if (!m_d2dContext) return;
    HRESULT hr = m_d2dContext->EndDraw();
    if (SUCCEEDED(hr)) {
        m_swapChain->Present(1, 0);
        m_dcompDevice->Commit();
    }
}

ComPtr<ID2D1SolidColorBrush> DCompLayer::CreateSolidBrush(const core::Color& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (m_d2dContext) {
        D2D1_COLOR_F d2dColor = D2D1::ColorF(color.r, color.g, color.b, color.a);
        m_d2dContext->CreateSolidColorBrush(d2dColor, &brush);
    }
    return brush;
}

ComPtr<ID2D1LinearGradientBrush> DCompLayer::CreateLinearGradientBrush(
    const core::Color& startCol, const core::Color& endCol,
    const D2D1_POINT_2F& startPt, const D2D1_POINT_2F& endPt) {
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (!m_d2dContext) return brush;

    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f;
    stops[0].color = D2D1::ColorF(startCol.r, startCol.g, startCol.b, startCol.a);
    stops[1].position = 1.0f;
    stops[1].color = D2D1::ColorF(endCol.r, endCol.g, endCol.b, endCol.a);

    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(stops, 2, &stopCollection))) {
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = D2D1::LinearGradientBrushProperties(startPt, endPt);
        m_d2dContext->CreateLinearGradientBrush(props, stopCollection.Get(), &brush);
    }
    return brush;
}

ComPtr<ID2D1RadialGradientBrush> DCompLayer::CreateRadialGradientBrush(
    const core::Color& innerCol, const core::Color& outerCol,
    const D2D1_POINT_2F& center, float radiusX, float radiusY) {
    ComPtr<ID2D1RadialGradientBrush> brush;
    if (!m_d2dContext) return brush;

    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f;
    stops[0].color = D2D1::ColorF(innerCol.r, innerCol.g, innerCol.b, innerCol.a);
    stops[1].position = 1.0f;
    stops[1].color = D2D1::ColorF(outerCol.r, outerCol.g, outerCol.b, outerCol.a);

    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (SUCCEEDED(m_d2dContext->CreateGradientStopCollection(stops, 2, &stopCollection))) {
        D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props = D2D1::RadialGradientBrushProperties(
            center, D2D1::Point2F(0, 0), radiusX, radiusY
        );
        m_d2dContext->CreateRadialGradientBrush(props, stopCollection.Get(), &brush);
    }
    return brush;
}

ComPtr<ID2D1StrokeStyle> DCompLayer::CreateRoundedStrokeStyle() {
    ComPtr<ID2D1StrokeStyle> style;
    if (!m_d2dFactory) return style;

    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND
    );
    m_d2dFactory->CreateStrokeStyle(&props, nullptr, 0, &style);
    return style;
}

ComPtr<ID2D1PathGeometry> DCompLayer::CreateSquircleGeometry(const core::Rect& bounds, float radius, float smoothing) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (!m_d2dFactory) return geometry;
    if (bounds.width <= 0.5f || bounds.height <= 0.5f) return geometry;

    if (FAILED(m_d2dFactory->CreatePathGeometry(&geometry))) { geometry.Reset(); return geometry; }

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) { geometry.Reset(); return geometry; }

    const float l = bounds.Left(),  t = bounds.Top();
    const float r = bounds.Right(), b = bounds.Bottom();

    const float budget = std::min(bounds.width, bounds.height) * 0.5f;
    const float R = std::clamp(radius, 0.0f, budget);

    if (R < 0.5f) {
        sink->BeginFigure(D2D1::Point2F(l, t), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(r, t));
        sink->AddLine(D2D1::Point2F(r, b));
        sink->AddLine(D2D1::Point2F(l, b));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return geometry;
    }

    // Figma-style corner smoothing: every corner is [cubic, arc, cubic] so curvature ramps up
    // gradually instead of jumping from 0 to 1/R like a plain rounded rectangle.
    const float maxSmoothing = budget / R - 1.0f;
    const float s = std::clamp(smoothing, 0.0f, std::max(0.0f, maxSmoothing));
    const float p = std::min((1.0f + s) * R, budget);

    constexpr float kDeg = 3.14159265358979f / 180.0f;
    const float arcMeasure = 90.0f * (1.0f - s);
    const float arcLen     = std::sin(arcMeasure * 0.5f * kDeg) * R * 1.41421356f;
    const float alphaDeg   = (90.0f - arcMeasure) * 0.5f;
    const float p3p4       = R * std::tan(alphaDeg * 0.5f * kDeg);
    const float betaDeg    = 45.0f * s;
    const float c          = p3p4 * std::cos(betaDeg * kDeg);
    const float d          = c * std::tan(betaDeg * kDeg);
    const float bb         = (p - arcLen - c - d) / 3.0f;
    const float a          = 2.0f * bb;

    struct Corner { D2D1_POINT_2F o; D2D1_POINT_2F e1; D2D1_POINT_2F e2; };
    // Clockwise: top-right, bottom-right, bottom-left, top-left.
    // e1 = direction we arrive travelling, e2 = direction we leave travelling.
    const Corner corners[4] = {
        { D2D1::Point2F(r, t), D2D1::Point2F( 1.0f,  0.0f), D2D1::Point2F( 0.0f,  1.0f) },
        { D2D1::Point2F(r, b), D2D1::Point2F( 0.0f,  1.0f), D2D1::Point2F(-1.0f,  0.0f) },
        { D2D1::Point2F(l, b), D2D1::Point2F(-1.0f,  0.0f), D2D1::Point2F( 0.0f, -1.0f) },
        { D2D1::Point2F(l, t), D2D1::Point2F( 0.0f, -1.0f), D2D1::Point2F( 1.0f,  0.0f) },
    };

    auto pt = [](const Corner& k, float u, float v) {
        return D2D1::Point2F(k.o.x + k.e1.x * u + k.e2.x * v, k.o.y + k.e1.y * u + k.e2.y * v);
    };

    for (int i = 0; i < 4; ++i) {
        const Corner& k = corners[i];
        const D2D1_POINT_2F start = pt(k, -p, 0.0f);
        if (i == 0) sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
        else        sink->AddLine(start);

        const float u3 = -p + a + bb + c;
        sink->AddBezier(D2D1::BezierSegment(pt(k, -p + a, 0.0f), pt(k, -p + a + bb, 0.0f), pt(k, u3, d)));

        const float u4 = u3 + arcLen;
        const float v4 = d + arcLen;
        sink->AddArc(D2D1::ArcSegment(pt(k, u4, v4), D2D1::SizeF(R, R), 0.0f,
                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));

        sink->AddBezier(D2D1::BezierSegment(pt(k, u4 + d, v4 + c), pt(k, u4 + d, v4 + bb + c), pt(k, 0.0f, p)));
    }

    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return geometry;
}

} // namespace aether::render
