#pragma once

#include "pch_engine.h"

// Helpers the live-capture providers share, and the colour contract every
// Source provider meets: the image a provider hands the graph is linear scRGB
// (1.0 = 80 nits), FP16 where the source allows it.
namespace ShaderLab::Effects
{
    // What one live-capture attempt produced.
    enum class CaptureResult
    {
        NewFrame,   // a new image is in CurrentBitmap()
        NoFrame,    // nothing new; the previous image stands
        Failed,     // the capture is lost or broken; LastError() says why
    };

    // "HRESULT 0x8007000E", for error text.
    std::wstring HResultMessage(HRESULT hr);

    // True for a surface format that already holds linear scRGB.
    bool IsLinearScRgbFormat(DXGI_FORMAT format);

    // A D3D11 texture the provider copies frames into, the D2D bitmap that
    // views it (no copy: the bitmap wraps the same surface), and the image
    // the graph reads. For an FP16 surface the image is the bitmap; for an
    // 8-bit surface, which holds sRGB-encoded values, it is the output of an
    // sRGB-to-linear effect over the bitmap.
    struct SharedOutput
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID2D1Bitmap1>    bitmap;
        winrt::com_ptr<ID2D1Effect>     decode;  // null for a linear surface
        winrt::com_ptr<ID2D1Image>      image;
        std::wstring                    error;   // set when any of them is missing

        void Reset() { *this = {}; }
    };

    // Creates a width x height SharedOutput of `format`. `what` names the
    // owner in the error text ("duplication", "WGC").
    SharedOutput CreateSharedOutput(ID2D1DeviceContext5* dc, ID3D11Device* device,
                                                  uint32_t width, uint32_t height, DXGI_FORMAT format,
                                                  std::wstring_view what);

    // The linear scRGB image for `bitmap`, whose surface has `format`: the
    // bitmap itself when the format is already linear, otherwise the output
    // of an sRGB-to-linear effect, returned in `decode` so the caller keeps
    // it alive. Null with `error` set on failure.
    winrt::com_ptr<ID2D1Image> AsLinearScRgb(ID2D1DeviceContext5* dc, ID2D1Bitmap1* bitmap,
                                                           DXGI_FORMAT format,
                                                           winrt::com_ptr<ID2D1Effect>& decode,
                                                           std::wstring& error);
}
