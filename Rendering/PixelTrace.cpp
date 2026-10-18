#include "pch_engine.h"
#include "PixelTrace.h"
#include "ImageCapture.h"
#include "../Effects/ColorMathCpu.h"

namespace ShaderLab::Rendering
{
    namespace
    {
        uint8_t ToSrgb8(float linear)
        {
            const float c = std::clamp(linear, 0.0f, 1.0f);
            const float encoded = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(encoded * 255.0f + 0.5f);
        }

        // One FP32 pixel of `image` at (x, y).
        HRESULT ReadOnePixel(ID2D1DeviceContext* dc, ID2D1Image* image, int32_t x, int32_t y, float (&rgba)[4])
        {
            const auto source = D2D1::RectF(static_cast<float>(x), static_cast<float>(y),
                                            static_cast<float>(x) + 1.0f, static_cast<float>(y) + 1.0f);
            PixelBuffer pixels;
            const HRESULT hr = RenderToPixels(dc, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D2D1::ColorF(0, 0, 0, 0),
                [&](ID2D1DeviceContext* target)
                {
                    const D2D1_POINT_2F origin = D2D1::Point2F(0.0f, 0.0f);
                    target->DrawImage(image, &origin, &source,
                        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
                },
                pixels);
            if (FAILED(hr)) return hr;
            std::memcpy(rgba, pixels.bytes.data(), sizeof(rgba));
            return S_OK;
        }

        struct TraceWalk
        {
            const Graph::GraphModel& graph;
            ID2D1DeviceContext* dc;
            int32_t x;
            int32_t y;
            std::unordered_map<uint32_t, std::optional<TracedPixel>> read;
            HRESULT firstError{ S_OK };

            std::optional<TracedPixel> PixelOf(const Graph::EffectNode& node)
            {
                if (auto it = read.find(node.id); it != read.end()) return it->second;
                std::optional<TracedPixel> pixel;
                if (node.cachedOutput)
                {
                    float rgba[4]{};
                    const HRESULT hr = ReadOnePixel(dc, node.cachedOutput, x, y, rgba);
                    if (SUCCEEDED(hr))
                        pixel = DescribePixel(rgba[0], rgba[1], rgba[2], rgba[3]);
                    else if (SUCCEEDED(firstError))
                        firstError = hr;
                }
                read.emplace(node.id, pixel);
                return pixel;
            }

            PixelTraceNode Visit(const Graph::EffectNode& node)
            {
                PixelTraceNode result;
                result.nodeId = node.id;
                result.nodeName = node.name;
                if (const auto pixel = PixelOf(node))
                {
                    result.hasPixel = true;
                    result.pixel = *pixel;
                }
                if (node.analysisOutput.type == Graph::AnalysisOutputType::Typed && !node.analysisOutput.fields.empty())
                {
                    result.hasAnalysisOutput = true;
                    result.analysisFields = node.analysisOutput.fields;
                }
                for (const auto* edge : graph.GetInputEdges(node.id))
                {
                    const auto* source = graph.FindNode(edge->sourceNodeId);
                    if (!source) continue;
                    auto child = Visit(*source);
                    child.inputPin = edge->destPin;
                    child.pinName = edge->destPin < node.inputPins.size()
                        ? node.inputPins[edge->destPin].name
                        : std::format(L"[{}]", edge->destPin);
                    result.inputs.push_back(std::move(child));
                }
                std::sort(result.inputs.begin(), result.inputs.end(),
                    [](const PixelTraceNode& a, const PixelTraceNode& b) { return a.inputPin < b.inputPin; });
                return result;
            }
        };
    }

    TracedPixel DescribePixel(float r, float g, float b, float a)
    {
        TracedPixel p;
        p.scR = r; p.scG = g; p.scB = b; p.scA = a;
        p.sR = ToSrgb8(r); p.sG = ToSrgb8(g); p.sB = ToSrgb8(b);
        p.sA = static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f));
        const auto hdr10 = Effects::ColorMathCpu::ScRGBToHdr10({ r, g, b });
        p.pqR = static_cast<float>(hdr10.x);
        p.pqG = static_cast<float>(hdr10.y);
        p.pqB = static_cast<float>(hdr10.z);
        p.luminanceNits = (0.2126f * r + 0.7152f * g + 0.0722f * b) * 80.0f;
        return p;
    }

    PixelTraceResult TracePixel(const Graph::GraphModel& graph, uint32_t nodeId,
                                int32_t x, int32_t y, ID2D1DeviceContext* dc)
    {
        PixelTraceResult result;
        const auto* node = graph.FindNode(nodeId);
        if (!node) return result;
        if (!dc)
        {
            result.hr = E_POINTER;
            return result;
        }
        TraceWalk walk{ graph, dc, x, y };
        result.root = walk.Visit(*node);
        result.hr = walk.firstError;
        return result;
    }
}
