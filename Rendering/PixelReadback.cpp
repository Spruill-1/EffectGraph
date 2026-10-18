#include "pch_engine.h"
#include "PixelReadback.h"
#include "ImageCapture.h"

#include <cstring>

namespace ShaderLab::Rendering
{
    ReadPixelRegionResult ReadPixelRegion(
        const Graph::GraphModel& graph,
        uint32_t nodeId,
        int32_t x, int32_t y, uint32_t w, uint32_t h,
        ID2D1DeviceContext* dc)
    {
        ReadPixelRegionResult result;
        if (!dc) { result.status = ReadPixelRegionStatus::D2DError; result.hr = E_POINTER; return result; }

        NodeImageStatus imageStatus = NodeImageStatus::NotFound;
        ID2D1Image* image = ResolveNodeImage(graph, nodeId, &imageStatus);
        if (!image)
        {
            result.status = imageStatus == NodeImageStatus::NotFound
                ? ReadPixelRegionStatus::NotFound : ReadPixelRegionStatus::NotReady;
            return result;
        }

        D2D1_RECT_F bounds{};
        result.hr = GetImageBounds96Dpi(dc, image, bounds);
        if (FAILED(result.hr)) { result.status = ReadPixelRegionStatus::D2DError; return result; }
        const int32_t imgW = static_cast<int32_t>(bounds.right - bounds.left);
        const int32_t imgH = static_cast<int32_t>(bounds.bottom - bounds.top);
        if (imgW <= 0 || imgH <= 0) { result.status = ReadPixelRegionStatus::InvalidRegion; return result; }

        // Clip to the image. `bounds.left/top` may be non-zero for sources
        // whose origin isn't at (0,0).
        const int32_t x0 = (std::max)(x, 0);
        const int32_t y0 = (std::max)(y, 0);
        const int32_t x1 = (std::min)(static_cast<int32_t>(x + static_cast<int64_t>(w)), imgW);
        const int32_t y1 = (std::min)(static_cast<int32_t>(y + static_cast<int64_t>(h)), imgH);
        if (x1 <= x0 || y1 <= y0) { result.status = ReadPixelRegionStatus::InvalidRegion; return result; }
        const uint32_t actW = static_cast<uint32_t>(x1 - x0);
        const uint32_t actH = static_cast<uint32_t>(y1 - y0);

        // FP32 so the values are linear scRGB as rendered. SOURCE_COPY with
        // nearest-neighbour copies them without blending or filtering, and a
        // source rect the size of the target samples 1:1.
        const D2D1_RECT_F sourceRect = D2D1::RectF(
            static_cast<float>(x0) + bounds.left, static_cast<float>(y0) + bounds.top,
            static_cast<float>(x1) + bounds.left, static_cast<float>(y1) + bounds.top);
        PixelBuffer pixels;
        result.hr = RenderToPixels(dc, actW, actH, DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1::ColorF(0, 0, 0, 0),
            [&](ID2D1DeviceContext* target)
            {
                const D2D1_POINT_2F origin = D2D1::Point2F(0.0f, 0.0f);
                target->DrawImage(image, &origin, &sourceRect,
                    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            },
            pixels);
        if (FAILED(result.hr)) { result.status = ReadPixelRegionStatus::D2DError; return result; }

        const size_t rowBytes = static_cast<size_t>(actW) * 4 * sizeof(float);
        result.pixels.resize(static_cast<size_t>(actW) * actH * 4);
        for (uint32_t row = 0; row < actH; ++row)
            std::memcpy(result.pixels.data() + static_cast<size_t>(row) * actW * 4,
                        pixels.bytes.data() + static_cast<size_t>(row) * pixels.pitch, rowBytes);
        result.actualWidth = actW;
        result.actualHeight = actH;
        result.status = ReadPixelRegionStatus::Success;
        return result;
    }
}
