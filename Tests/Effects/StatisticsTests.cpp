#include "pch_engine.h"
#include "../TestSupport.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/SourceRuntime.h"
#include "Graph/GraphEdits.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Luminance Statistics: the reduction, clip counting and highlight
// percentiles.
namespace
{
    using namespace ShaderLab::Tests;

    // Direct exercise of Luminance Statistics (D3D11 compute path) so we can
    // tell whether a Mean=0 readback is a Luminance Statistics-specific bug
    // or something the headless host introduces.
    SL_TEST_CASE(TestLuminanceStatistics, Tag::Gpu)
    {
        printf("\n=== Luminance Statistics (D3D11 compute) ===\n");
        // CaseEvaluator() is a global; node IDs (1, 2, ...) collide with
        // prior tests' graphs, so flush its caches before building a
        // fresh graph or CreateOrGetEffect will cache-hit a stale
        // effect from a different test.
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* lumDesc = registry.FindByName(L"Luminance Statistics");
        if (!lumDesc) { TEST("LumStatsDescriptorFound", false); return; }

        ShaderLab::Graph::GraphModel g;
        ShaderLab::Effects::SourceNodeFactory sf;
        auto srcNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gamut Source"));
        auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*lumDesc);
        auto srcId = g.AddNode(std::move(srcNode));
        auto statsId = g.AddNode(std::move(statsNode));
        g.Connect(srcId, 0, statsId, 0);
        PrepareSourcesAndEvaluate(g, sf);
        // D2D custom effects need TWO evaluation passes for newly
        // created effects -- first creates/initializes, second
        // produces correct output.
        g_dc->SetTarget(nullptr);
        CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
        PrepareSourcesAndEvaluate(g, sf);
        CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

        auto* n = g.FindNode(statsId);
        TEST("LumStats_NodeExists", n != nullptr);
        if (!n) return;
        TEST("LumStats_AnalysisFieldsPopulated", !n->analysisOutput.fields.empty());

        float mean = 0.0f, maxVal = 0.0f;
        for (const auto& f : n->analysisOutput.fields) {
            if (f.name == L"Mean") mean = f.components[0];
            if (f.name == L"Max")  maxVal = f.components[0];
        }
        // Gamut Source at Luminance=80 covers part of the 64x64 image with
        // gamut-shaped fill. Max should reach ~80 nits; Mean should be a
        // small but positive fraction of that. Both being 0 is the
        // "DrawImage outside draw session" failure mode.
        TEST("LumStats_MaxIsPositive", maxVal > 0.0f);
        TEST("LumStats_MeanIsPositive", mean > 0.0f);
        if (mean == 0.0f || maxVal == 0.0f) {
            printf("    (debug) Mean=%.4f Max=%.4f -- D3D11 dispatch likely not reading source\n",
                mean, maxVal);
        }
    }

    // Luminance Statistics counts a pixel as clipped only when it is more
    // than 1% above ClipNits. SDR white stored as W/80 sits at W, or an FP16
    // step above it, and must not count.
    SL_TEST_CASE(TestLuminanceClipMargin, Tag::Gpu)
    {
        printf("\n=== Luminance Statistics: ClippedFraction margin ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        // Every value is exact in FP16, so the CPU mirror needs no rounding.
        constexpr float cWhite = 3.5f;                 // 280 nits
        constexpr float cWhiteNextHalf = 3.501953125f; // one FP16 step above white
        constexpr float cHighlight = 5.25f;            // 1.5 x white
        constexpr float cJustAbove = 3.537109375f;     // 1.0106 x white
        constexpr float cJustBelow = 3.533203125f;     // 1.0095 x white
        constexpr float cGrey = 1.75f;
        constexpr int cWidth = 256;
        constexpr int cHeight = 16;
        constexpr float cClipNits = 280.0f;
        constexpr float cBudgetScale = 15.0f;
        auto valueAt = [&](int x) -> float
        {
            if (x < 128) return cWhite;
            if (x < 160) return cWhiteNextHalf;
            if (x < 192) return cHighlight;
            if (x < 208) return cJustAbove;
            if (x < 224) return cJustBelow;
            return cGrey;
        };
        const auto pattern = PatternEffect(L"Clip Margin Test Pattern",
            "    float v = pixel.x < 128.0 ? 3.5 : pixel.x < 160.0 ? 3.501953125 : pixel.x < 192.0 ? 5.25\n"
            "            : pixel.x < 208.0 ? 3.537109375 : pixel.x < 224.0 ? 3.533203125 : 1.75;\n"
            "    return float4(v, v, v, input.a);\n");

        ShaderLab::Effects::SourceNodeFactory sourceFactory;
        ShaderLab::Graph::GraphModel graph;
        const auto patternId = AddPatternSource(graph, sourceFactory, pattern, static_cast<float>(cWidth), static_cast<float>(cHeight));
        const auto statsId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        ShaderLab::Graph::SetNodeProperty(*graph.FindNode(statsId), L"ClipNits", cClipNits);
        ShaderLab::Graph::SetNodeProperty(*graph.FindNode(statsId), L"BudgetScale", cBudgetScale);
        graph.Connect(patternId, 0, statsId, 0);
        graph.MarkAllDirty();
        RunFrame(graph);
        graph.MarkAllDirty();
        RunFrame(graph);

        // CPU mirror of the shader's rule.
        int clipped = 0;
        int clippedAtOrAbove = 0;
        for (int x = 0; x < cWidth; ++x)
        {
            const float value = valueAt(x);
            const float nits = (value * 0.2126f + value * 0.7152f + value * 0.0722f) * 80.0f;
            if (nits > cClipNits * 1.01f) clipped += cHeight;
            if (nits >= cClipNits) clippedAtOrAbove += cHeight;
        }
        const float cpuFraction = static_cast<float>(clipped) / (cWidth * cHeight);

        const auto* statsNode = graph.FindNode(statsId);
        const float gpuFraction = AnalysisField(statsNode, L"ClippedFraction");
        const float budget = AnalysisField(statsNode, L"WhiteErrorBudget");
        printf("  [info] ClippedFraction GPU %.6f CPU %.6f (>= ClipNits would give %.6f), budget %.4f\n",
            gpuFraction, cpuFraction, static_cast<float>(clippedAtOrAbove) / (cWidth * cHeight), budget);
        TEST("LumClip_SamplesCounted", AnalysisField(statsNode, L"Samples") == static_cast<float>(cWidth * cHeight));
        TEST("LumClip_OnlyClearlyAboveWhiteCounts", cpuFraction == 48.0f / 256.0f);
        TEST("LumClip_GpuMatchesCpu", gpuFraction == cpuFraction);
        TEST("LumClip_BudgetFollowsFraction", std::abs(budget - cpuFraction * cBudgetScale) < 1e-4f);
        CaseEvaluator().ReleaseCache();
    }

    // HighlightP99 is the 99th percentile of the above-white pixels alone:
    // it must find a small HDR window's highlights that the whole-frame P99
    // never reaches, match a CPU mirror of the histogram on a full ramp, and
    // fall back to the clip threshold when nothing is above white.
    SL_TEST_CASE(TestLuminanceHighlightP99, Tag::Gpu)
    {
        printf("\n=== Luminance Statistics: HighlightP99 ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        constexpr int cWidth = 256;
        constexpr int cHeight = 16;
        constexpr float cClipNits = 280.0f;

        auto measure = [&](const wchar_t* name, const char* body, float& highlightP99, float& p99)
        {
            ShaderLab::Effects::SourceNodeFactory sourceFactory;
            ShaderLab::Graph::GraphModel graph;
            const auto pattern = PatternEffect(name, body);
            const auto patternId = AddPatternSource(graph, sourceFactory, pattern, static_cast<float>(cWidth), static_cast<float>(cHeight));
            const auto statsId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            ShaderLab::Graph::SetNodeProperty(*graph.FindNode(statsId), L"ClipNits", cClipNits);
            graph.Connect(patternId, 0, statsId, 0);
            graph.MarkAllDirty();
            RunFrame(graph);
            graph.MarkAllDirty();
            RunFrame(graph);
            highlightP99 = AnalysisField(graph.FindNode(statsId), L"HighlightP99");
            p99 = AnalysisField(graph.FindNode(statsId), L"P99");
            CaseEvaluator().ReleaseCache();
        };
        // The shader's bin centre for a value, by the same rule. Adjacent bins
        // are 7.5% apart, so 0.1% pins the bin and allows for the GPU's pow.
        auto binCentre = [](float nits)
        {
            const float t = (std::log10((std::max)(nits, 1e-4f)) + 2.0f) / 8.0f;
            const int bin = static_cast<int>((std::min)((std::max)(t, 0.0f), 1.0f) * 255.0f);
            return std::pow(10.0f, -2.0f + (bin + 0.5f) / 256.0f * 8.0f);
        };
        const float clipThreshold = cClipNits * 1.01f;

        // A small HDR window: two columns, 500 and 1500 nits, in an 80-nit frame (0.8%).
        float windowHighlight = 0.0f, windowP99 = 0.0f;
        measure(L"Highlight Window Pattern",
            "    float v = pixel.x < 254.0 ? 1.0 : pixel.x < 255.0 ? 6.25 : 18.75;\n"
            "    return float4(v, v, v, input.a);\n", windowHighlight, windowP99);
        printf("  [info] small window: P99 %.1f, HighlightP99 %.1f (expect bin of 1500 = %.1f)\n",
            windowP99, windowHighlight, binCentre(1500.0f));
        TEST("LumHighlightP99_WholeFrameP99StaysSdr", windowP99 < cClipNits);
        TEST("LumHighlightP99_FindsSmallWindow", std::abs(windowHighlight / binCentre(1500.0f) - 1.0f) < 1e-3f);

        // A full ramp: columns 128..255 at 300 + 10 nits per column. 2048
        // highlight pixels, so 20 may lie above the percentile: the top
        // column (16) and four of the next, which therefore holds it.
        float rampHighlight = 0.0f, rampP99 = 0.0f;
        measure(L"Highlight Ramp Pattern",
            "    float v = pixel.x < 128.0 ? 1.0 : 3.75 + (pixel.x - 128.0) * 0.125;\n"
            "    return float4(v, v, v, input.a);\n", rampHighlight, rampP99);
        std::vector<float> highlights;
        for (int x = 128; x < cWidth; ++x)
            for (int y = 0; y < cHeight; ++y)
                highlights.push_back((300.0f + (x - 128) * 10.0f));
        std::sort(highlights.begin(), highlights.end());
        const size_t tail = highlights.size() / 100;
        const float expected = binCentre(highlights[highlights.size() - 1 - tail]);
        printf("  [info] ramp: P99 %.1f, HighlightP99 %.1f, CPU mirror %.1f (value %.0f)\n",
            rampP99, rampHighlight, expected, highlights[highlights.size() - 1 - tail]);
        TEST("LumHighlightP99_MatchesCpuMirror", std::abs(rampHighlight / expected - 1.0f) < 1e-3f);

        // Nothing above white: the clip threshold itself.
        float noneHighlight = 0.0f, noneP99 = 0.0f;
        measure(L"Highlight None Pattern",
            "    float v = pixel.x < 128.0 ? 1.0 : 3.5;\n"
            "    return float4(v, v, v, input.a);\n", noneHighlight, noneP99);
        printf("  [info] no highlights: HighlightP99 %.2f (clip threshold %.2f)\n", noneHighlight, clipThreshold);
        TEST("LumHighlightP99_NoneIsClipThreshold", std::abs(noneHighlight - clipThreshold) < 1e-2f);
    }
}
