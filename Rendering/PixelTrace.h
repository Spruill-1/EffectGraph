#pragma once

#include "pch_engine.h"
#include "../Graph/GraphModel.h"

// The value of one pixel at a node and at every node upstream of it, for the
// Pixel Trace panel and the /render/pixel-trace MCP route.
namespace ShaderLab::Rendering
{
    // One pixel in the representations the trace shows.
    struct TracedPixel
    {
        float scR{ 0 }, scG{ 0 }, scB{ 0 }, scA{ 0 };   // linear scRGB, as rendered
        uint8_t sR{ 0 }, sG{ 0 }, sB{ 0 }, sA{ 0 };     // sRGB-encoded, clipped to [0, 1]
        float pqR{ 0 }, pqG{ 0 }, pqB{ 0 };             // HDR10: BT.2020 primaries, PQ
        float luminanceNits{ 0 };                       // Rec.709 Y x 80
    };

    TracedPixel DescribePixel(float r, float g, float b, float a);

    struct PixelTraceNode
    {
        uint32_t nodeId{ 0 };
        std::wstring nodeName;
        uint32_t inputPin{ 0 };     // the parent's pin this node feeds
        std::wstring pinName;
        // False when the node has no output yet or its read failed.
        bool hasPixel{ false };
        TracedPixel pixel;
        bool hasAnalysisOutput{ false };
        std::vector<Graph::AnalysisFieldValue> analysisFields;
        std::vector<PixelTraceNode> inputs;   // upstream, by pin
    };

    struct PixelTraceResult
    {
        std::optional<PixelTraceNode> root;   // empty when nodeId is not in the graph
        // The first failed read (EndDraw's included), else S_OK.
        HRESULT hr{ S_OK };
    };

    // Reads the pixel at (x, y), in the 96-DPI coordinates every image in a
    // D2D effect chain shares, from nodeId's output and recursively from
    // each node feeding it through image edges. A node reached by several
    // paths is read once. Render thread, outside a draw session.
    PixelTraceResult TracePixel(const Graph::GraphModel& graph, uint32_t nodeId,
                                              int32_t x, int32_t y, ID2D1DeviceContext* dc);
}
