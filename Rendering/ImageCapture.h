#pragma once

#include "pch_engine.h"

#include <functional>

namespace ShaderLab::Graph
{
    class GraphModel;
}

// Reading a rendered image back to the CPU and encoding it. Every readback in
// the engine and the GUI goes through these, so each saves and restores the
// context the same way and reports the same failures.
namespace ShaderLab::Rendering
{
    enum class NodeImageStatus
    {
        Ready,
        NotFound,   // no node with that id
        NotReady,   // dirty (its cachedOutput may be released), or a required input is unwired
    };

    // A node's output, safe to draw now, or nullptr with the reason.
    ID2D1Image* ResolveNodeImage(const Graph::GraphModel& graph, uint32_t nodeId,
                                               NodeImageStatus* status = nullptr);

    // An image's bounds in pixels: measured at 96 DPI, the context's DPI restored.
    HRESULT GetImageBounds96Dpi(ID2D1DeviceContext* dc, ID2D1Image* image, D2D1_RECT_F& bounds);

    // Pixels copied to the CPU, `pitch` bytes per row.
    struct PixelBuffer
    {
        uint32_t             width{ 0 };
        uint32_t             height{ 0 };
        uint32_t             pitch{ 0 };
        DXGI_FORMAT          format{ DXGI_FORMAT_UNKNOWN };
        std::vector<uint8_t> bytes;
    };

    // Runs draw(dc) into a width x height bitmap of `format` (premultiplied
    // alpha, 96 DPI, identity transform, cleared to clear) in a draw session
    // of its own, then copies the bitmap to `out`. Must not be called inside a
    // draw session on `dc`. The context's target, DPI and transform are
    // restored on every path. Returns the first failing call's HRESULT, or
    // EndDraw's.
    HRESULT RenderToPixels(ID2D1DeviceContext* dc, uint32_t width, uint32_t height,
                                         DXGI_FORMAT format, const D2D1_COLOR_F& clear,
                                         const std::function<void(ID2D1DeviceContext*)>& draw,
                                         PixelBuffer& out);

    // Encodes 8-bit BGRA pixels as PNG into memory.
    HRESULT EncodePng(const PixelBuffer& pixels, std::vector<uint8_t>& png);

    // Encodes pixels to a file: 8-bit BGRA as PNG, R16G16B16A16_FLOAT as
    // lossless JPEG XR tagged scRGB.
    HRESULT EncodeToFile(const PixelBuffer& pixels, const std::wstring& path);
}
