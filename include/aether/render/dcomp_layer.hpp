#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include "aether/core/types.hpp"

namespace aether::render {

using Microsoft::WRL::ComPtr;

class DCompLayer {
public:
    DCompLayer();
    ~DCompLayer();

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Resize(uint32_t width, uint32_t height);
    void Cleanup();

    [[nodiscard]] ID2D1DeviceContext* BeginDraw();
    void EndDraw();

    [[nodiscard]] IDWriteFactory* GetDWriteFactory() const { return m_dwriteFactory.Get(); }
    [[nodiscard]] ID2D1Factory1* GetD2DFactory() const { return m_d2dFactory.Get(); }

    [[nodiscard]] uint32_t GetWidth() const { return m_width; }
    [[nodiscard]] uint32_t GetHeight() const { return m_height; }

    // Direct2D brush & geometry helpers
    ComPtr<ID2D1SolidColorBrush> CreateSolidBrush(const core::Color& color);
    ComPtr<ID2D1LinearGradientBrush> CreateLinearGradientBrush(
        const core::Color& startCol, const core::Color& endCol,
        const D2D1_POINT_2F& startPt, const D2D1_POINT_2F& endPt
    );
    ComPtr<ID2D1RadialGradientBrush> CreateRadialGradientBrush(
        const core::Color& innerCol, const core::Color& outerCol,
        const D2D1_POINT_2F& center, float radiusX, float radiusY
    );
    ComPtr<ID2D1StrokeStyle> CreateRoundedStrokeStyle();
    ComPtr<ID2D1PathGeometry> CreateSquircleGeometry(const core::Rect& bounds, float radius);

    [[nodiscard]] ID2D1DeviceContext* GetContext() const { return m_d2dContext.Get(); }

private:
    bool CreateDeviceResources();
    bool CreateSwapChain(uint32_t width, uint32_t height);

    HWND m_hwnd{nullptr};
    uint32_t m_width{0};
    uint32_t m_height{0};

    ComPtr<ID3D11Device> m_d3d11Device;
    ComPtr<ID3D11DeviceContext> m_d3d11Context;
    ComPtr<IDXGISwapChain1> m_swapChain;

    ComPtr<ID2D1Factory1> m_d2dFactory;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<ID2D1Bitmap1> m_d2dTargetBitmap;

    ComPtr<IDWriteFactory> m_dwriteFactory;

    ComPtr<IDCompositionDevice> m_dcompDevice;
    ComPtr<IDCompositionTarget> m_dcompTarget;
    ComPtr<IDCompositionVisual> m_dcompVisual;
};

} // namespace aether::render
