#pragma once

#include "pch_engine.h"
#include "../Effects/CustomComputeBridgeEffect.h"

// Small scoped helpers for device-context state that has to be restored on
// every exit path.
namespace ShaderLab::Rendering
{
    // Holds a device context at 96 DPI, so image bounds come back in pixels.
    // Touches the DPI only when it differs: a real change drops every
    // D2D1_PROPERTY_CACHED intermediate in the context.
    class ScopedDpi96
    {
    public:
        explicit ScopedDpi96(ID2D1DeviceContext* dc) : m_dc(dc)
        {
            m_dc->GetDpi(&m_dpiX, &m_dpiY);
            m_changed = m_dpiX != 96.0f || m_dpiY != 96.0f;
            if (m_changed) m_dc->SetDpi(96.0f, 96.0f);
        }
        ~ScopedDpi96()
        {
            if (m_changed) m_dc->SetDpi(m_dpiX, m_dpiY);
        }
        ScopedDpi96(const ScopedDpi96&) = delete;
        ScopedDpi96& operator=(const ScopedDpi96&) = delete;

    private:
        ID2D1DeviceContext* m_dc;
        float m_dpiX{ 96.0f };
        float m_dpiY{ 96.0f };
        bool  m_changed{ false };
    };

    // Points a device context at `target` and puts the previous target back.
    class ScopedTarget
    {
    public:
        ScopedTarget(ID2D1DeviceContext* dc, ID2D1Image* target) : m_dc(dc)
        {
            m_dc->GetTarget(m_previous.put());
            m_dc->SetTarget(target);
        }
        ~ScopedTarget() { m_dc->SetTarget(m_previous.get()); }
        ScopedTarget(const ScopedTarget&) = delete;
        ScopedTarget& operator=(const ScopedTarget&) = delete;

    private:
        ID2D1DeviceContext* m_dc;
        winrt::com_ptr<ID2D1Image> m_previous;
    };

    // The whole-pixel rect `image` covers at the context's current DPI
    // (Effects::SnapComputeInputRect says which rects fail and why).
    inline HRESULT GetSnappedPixelBounds(ID2D1DeviceContext* dc, ID2D1Image* image, D2D1_RECT_L& out)
    {
        D2D1_RECT_F bounds{};
        HRESULT hr = dc->GetImageLocalBounds(image, &bounds);
        if (SUCCEEDED(hr))
            hr = Effects::SnapComputeInputRect(bounds, out);
        return hr;
    }

    // The texture behind `image` when a compute shader can read it in place:
    // a premultiplied FP16 or FP32 bitmap covering exactly `px` from the
    // origin, whose surface is a whole single-sample shader-readable texture
    // of the same size (not a slice of a D2D atlas). Null when the image has
    // to be drawn into an FP32 copy first.
    inline winrt::com_ptr<ID3D11Texture2D> ComputeReadableTexture(ID2D1Image* image, const D2D1_RECT_L& px)
    {
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (FAILED(image->QueryInterface(bitmap.put())) || !bitmap) return nullptr;

        const auto format = bitmap->GetPixelFormat();
        const auto size = bitmap->GetPixelSize();
        if (format.alphaMode != D2D1_ALPHA_MODE_PREMULTIPLIED) return nullptr;
        if (format.format != DXGI_FORMAT_R32G32B32A32_FLOAT &&
            format.format != DXGI_FORMAT_R16G16B16A16_FLOAT) return nullptr;
        if (px.left != 0 || px.top != 0 ||
            static_cast<UINT32>(px.right) != size.width || static_cast<UINT32>(px.bottom) != size.height)
            return nullptr;

        winrt::com_ptr<IDXGISurface> surface;
        if (FAILED(bitmap->GetSurface(surface.put())) || !surface) return nullptr;
        auto texture = surface.try_as<ID3D11Texture2D>();
        if (!texture) return nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width != size.width || desc.Height != size.height || desc.Format != format.format ||
            desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
            !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
            return nullptr;
        return texture;
    }
}
