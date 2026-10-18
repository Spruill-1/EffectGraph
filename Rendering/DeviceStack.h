#pragma once

#include "pch_engine.h"

// The D3D11 + D2D device stack every host renders on, created one way: BGRA
// support for D2D interop, D3D10 multithread protection (video decode and the
// compute runner submit from other threads) and a MULTI_THREADED D2D factory
// (capture providers deliver frames on background threads).
namespace ShaderLab::Rendering
{
    enum class AdapterChoice
    {
        Default,    // hardware, falling back to WARP
        Hardware,   // hardware only
        Warp,       // the WARP software rasterizer
        Adapter,    // the adapter with DeviceStackOptions::adapterLuid, else Default
    };

    struct DeviceStackOptions
    {
        AdapterChoice choice{ AdapterChoice::Default };
        LUID          adapterLuid{};
    };

    struct DeviceStack
    {
        winrt::com_ptr<IDXGIFactory7>        dxgiFactory;
        winrt::com_ptr<ID3D11Device5>        d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext4> d3dContext;
        winrt::com_ptr<ID2D1Factory7>        d2dFactory;
        winrt::com_ptr<ID2D1Device6>         d2dDevice;
        std::wstring adapterName;
        bool         isWarp{ false };

        // A new D2D device context on this stack's device. Each thread that
        // draws needs its own: target, transform and DPI are per context.
        HRESULT CreateDeviceContext(winrt::com_ptr<ID2D1DeviceContext5>& context) const
        {
            context = nullptr;
            if (!d2dDevice) return E_POINTER;
            winrt::com_ptr<ID2D1DeviceContext> base;
            const HRESULT hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, base.put());
            if (FAILED(hr)) return hr;
            return base->QueryInterface(IID_PPV_ARGS(context.put()));
        }
    };

    // Creates the stack. On failure returns the HRESULT and, when given,
    // names the step that failed in failedStep.
    HRESULT CreateDeviceStack(const DeviceStackOptions& options, DeviceStack& stack,
                                            std::wstring* failedStep = nullptr);

    // Builds the rest of the stack on a device the caller created: its DXGI
    // factory, multithread protection, and a D2D factory and device. The
    // device must have D3D11_CREATE_DEVICE_BGRA_SUPPORT.
    HRESULT CreateDeviceStackOn(ID3D11Device* device, DeviceStack& stack,
                                              std::wstring* failedStep = nullptr);
}
