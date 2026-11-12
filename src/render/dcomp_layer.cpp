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

ComPtr<ID2D1PathGeometry> DCompLayer::CreateSquircleGeometry(const core::Rect& bounds, float radius) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (!m_d2dFactory) return geometry;

    if (FAILED(m_d2dFactory->CreatePathGeometry(&geometry))) return geometry;

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return geometry;

    float x = bounds.Left();
    float y = bounds.Top();
    float w = bounds.width;
    float h = bounds.height;

    // Squircle curvature blend factor (Apple G2 curvature continuity)
    float k = std::min(radius * 1.5286f, std::min(w, h) * 0.5f);
    float cpRatio = 0.45f;

    // Start at top edge
    sink->BeginFigure(D2D1::Point2F(x + k, y), D2D1_FIGURE_BEGIN_FILLED);

    // Top edge -> Top-right corner
    sink->AddLine(D2D1::Point2F(x + w - k, y));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(x + w - k * cpRatio, y),
        D2D1::Point2F(x + w, y + k * cpRatio),
        D2D1::Point2F(x + w, y + k)
    ));

    // Right edge -> Bottom-right corner
    sink->AddLine(D2D1::Point2F(x + w, y + h - k));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(x + w, y + h - k * cpRatio),
        D2D1::Point2F(x + w - k * cpRatio, y + h),
        D2D1::Point2F(x + w - k, y + h)
    ));

    // Bottom edge -> Bottom-left corner
    sink->AddLine(D2D1::Point2F(x + k, y + h));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(x + k * cpRatio, y + h),
        D2D1::Point2F(x, y + h - k * cpRatio),
        D2D1::Point2F(x, y + h - k)
    ));

    // Left edge -> Top-left corner
    sink->AddLine(D2D1::Point2F(x, y + k));
    sink->AddBezier(D2D1::BezierSegment(
        D2D1::Point2F(x, y + k * cpRatio),
        D2D1::Point2F(x + k * cpRatio, y),
        D2D1::Point2F(x + k, y)
    ));

    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    return geometry;
}

} // namespace aether::render
