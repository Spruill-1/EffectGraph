#include "pch_engine.h"
#include "CaptureNode.h"
#include "ImageCapture.h"

#include "../Graph/GraphModel.h"

namespace ShaderLab::Rendering
{
    CaptureNodeResult CaptureNodeAsPng(
        Graph::GraphModel& graph,
        uint32_t nodeId,
        ID2D1DeviceContext* dc,
        uint32_t maxDim)
    {
        CaptureNodeResult result;
        if (!dc) { result.status = CaptureNodeStatus::D2DError; result.hr = E_POINTER; return result; }

        NodeImageStatus imageStatus = NodeImageStatus::NotFound;
        ID2D1Image* image = ResolveNodeImage(graph, nodeId, &imageStatus);
        if (!image)
        {
            result.status = imageStatus == NodeImageStatus::NotFound
                ? CaptureNodeStatus::NotFound : CaptureNodeStatus::NotReady;
            return result;
        }

        D2D1_RECT_F bounds{};
        result.hr = GetImageBounds96Dpi(dc, image, bounds);
        if (FAILED(result.hr)) { result.status = CaptureNodeStatus::D2DError; return result; }
        const float srcW = bounds.right - bounds.left;
        const float srcH = bounds.bottom - bounds.top;
        if (srcW <= 0.f || srcH <= 0.f) { result.status = CaptureNodeStatus::EmptyImage; return result; }

        // Fit the longer edge to maxDim, keeping the aspect ratio and never
        // upscaling, so the whole frame is captured.
        const float scale = (std::min)(1.0f, static_cast<float>(maxDim) / (std::max)(srcW, srcH));
        const uint32_t w = (std::max)(1u, static_cast<uint32_t>(srcW * scale + 0.5f));
        const uint32_t h = (std::max)(1u, static_cast<uint32_t>(srcH * scale + 0.5f));

        // An _SRGB target gamma-encodes the linear scRGB scene on write, so
        // the PNG bytes are display-ready (pairs with the loader's decode).
        PixelBuffer pixels;
        result.hr = RenderToPixels(dc, w, h, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, D2D1::ColorF(D2D1::ColorF::Black),
            [&](ID2D1DeviceContext* target)
            {
                target->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top) *
                                     D2D1::Matrix3x2F::Scale(scale, scale));
                target->DrawImage(image);
            },
            pixels);
        if (SUCCEEDED(result.hr))
            result.hr = EncodePng(pixels, result.png);
        if (FAILED(result.hr)) { result.status = CaptureNodeStatus::D2DError; return result; }

        result.status = CaptureNodeStatus::Success;
        result.width = w;
        result.height = h;
        return result;
    }
}
