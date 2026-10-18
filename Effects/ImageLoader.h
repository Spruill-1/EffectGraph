#pragma once

#include "pch_engine.h"

namespace ShaderLab::Effects
{
    // Loads image files from disk via WIC and converts them to ID2D1Bitmap1
    // suitable for use as source images in the effect graph.
    //
    // Contract: EVERY image the loader returns is a linear scRGB FP16
    // bitmap (R16G16B16A16_FLOAT, 1.0 = 80 nits). WIC hands us encoded
    // pixels; the D2D ColorManagement effect performs the transfer decode
    // and primaries conversion at float precision, honoring an embedded
    // ICC profile when present and otherwise assuming per-format:
    //   - 8-bit and 16-bit integer formats → sRGB
    //   - 10-bit 1010102 formats           → HDR10 (PQ / BT.2020)
    //   - float / half formats             → already linear scRGB (no-op)
    // The result is flattened once at load, so downstream consumers see a
    // single canonical format regardless of source. When any step fails the
    // load fails: there is no "best effort" encoded bitmap.
    class ImageLoader
    {
    public:
        // Does not touch COM; the WIC factory is created on the first load.
        ImageLoader() = default;

        // Load an image from a file path. Returns a linear scRGB FP16 bitmap,
        // or nullptr with LastError() naming the failed stage and HRESULT.
        // Must be called outside a BeginDraw on `dc`: the colour-managed
        // image is flattened through a temporary target.
        winrt::com_ptr<ID2D1Bitmap1> LoadFromFile(
            const std::wstring& filePath,
            ID2D1DeviceContext5* dc);

        // Why the last LoadFromFile returned nullptr; empty after a success.
        const std::wstring& LastError() const { return m_lastError; }

        // Every file extension WIC can decode on THIS machine, lowercased,
        // dot-prefixed, sorted and de-duplicated (".avif", ".heic", ".jpg", ...).
        // Read from the registered WIC decoders: codecs arrive as OS
        // extensions (HEIF, AVIF) and third-party packages (camera RAW), so
        // the set is machine state. Falls back to the in-box codecs if
        // enumeration fails, so the picker is never empty.
        static std::vector<std::wstring> SupportedExtensions();

    private:
        bool EnsureFactory();

        // Decode one WIC frame into the canonical linear scRGB FP16
        // bitmap (see class comment). Must be called OUTSIDE an active
        // BeginDraw on `dc` — it flattens through a temporary target.
        winrt::com_ptr<ID2D1Bitmap1> CreateScRgbBitmap(
            IWICBitmapFrameDecode* frame,
            ID2D1DeviceContext5* dc);

        winrt::com_ptr<ID2D1Bitmap1> Fail(const wchar_t* stage, HRESULT hr);

        winrt::com_ptr<IWICImagingFactory2> m_wicFactory;
        std::wstring m_lastError;
    };
}
