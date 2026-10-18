#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/EffectRegistry.h"
#include "Effects/Performance.h"
#include "Effects/ShaderLabParamsHlsl.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/D2DHelpers.h"
#include "Rendering/PixelReadback.h"
#include "Graph/GraphEdits.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// D3D11 compute through the evaluator: chain freshness, dispatch counts,
// wide frames and the derived-constant memo.
namespace
{
    using namespace ShaderLab::Tests;

    // A D3D11 compute node fed by another compute node reads this frame's
    // producer output, not the previous evaluation's: sweeping an upstream
    // parameter moves the consumer in the same step. Dispatches once per sweep,
    // as headless renders; a second dispatch would hide a one-step lag.
    SL_TEST_CASE(TestComputeChainFreshness, Tag::Gpu)
    {
        printf("\n=== Compute Chain Freshness ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        auto* srcDesc  = registry.FindByName(L"Gamut Source");
        auto* prodDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        auto* consDesc = registry.FindByName(L"ICtCp Highlight Desaturation");
        if (!srcDesc || !prodDesc || !consDesc)
        {
            TEST("ComputeChain_FindDescriptors", false);
            return;
        }

        ShaderLab::Graph::GraphModel g;
        ShaderLab::Effects::SourceNodeFactory sf;
        auto srcId  = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto prodId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*prodDesc));
        auto consId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*consDesc));
        g.Connect(srcId, 0, prodId, 0);
        g.Connect(prodId, 0, consId, 0);

        // Gamut Source renders black outside its gamut triangle, so sample the
        // D65 centre of the default 1024px output -- a corner reads 0 and tone
        // maps to 0 at every peak, which would make the sweep look static for
        // reasons that have nothing to do with the bug under test. 400 nits
        // puts the sample well above the tone curve's knee so the parameter
        // actually bites.
        ShaderLab::Graph::SetNodeProperty(*g.FindNode(srcId), L"Luminance", 400.0f);
        constexpr int32_t kProbeX = 512, kProbeY = 512;

        // One FrameExecutor frame per sweep: a single dispatch, then the
        // frozen sweep that clears the dirty flags it set (without it the
        // readback below returns NotReady).
        // Warm-up: new D2D custom effects need two evaluation passes before
        // their output is correct, so settle the chain before measuring.
        RenderFrame(g, sf);
        RenderFrame(g, sf);

        auto sweep = [&](float targetPeak) -> float {
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(prodId), L"TargetPeakNits", targetPeak);
            RenderFrame(g, sf);
            auto r = ShaderLab::Rendering::ReadPixelRegion(g, consId, kProbeX, kProbeY, 1, 1, g_dc.get());
            if (r.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success ||
                r.pixels.size() < 4)
            {
                printf("      readback failed: status=%d pixels=%zu cachedOutput=%s\n",
                    static_cast<int>(r.status), r.pixels.size(),
                    g.FindNode(consId)->cachedOutput ? "set" : "null");
                return -1.0f;
            }
            return r.pixels[1];   // green channel
        };

        const float a1 = sweep(203.0f);
        const float b  = sweep(100.0f);
        const float a2 = sweep(203.0f);

        const bool readOk = a1 >= 0.0f && b >= 0.0f && a2 >= 0.0f;
        TEST("ComputeChain_ReadbackSucceeded", readOk);
        // Guard the value assertions on the readback: without this, a failed
        // read returns -1 from every sweep and the equality check below passes
        // vacuously -- a false pass that would hide the very bug it pins.
        // The consumer must respond to its producer's parameter at all. With
        // the lag, the 100 read returns the 203 result and these are equal.
        TEST("ComputeChain_DownstreamTracksUpstream", readOk && std::fabs(a1 - b) > 1e-3f);
        // Returning to the first setting must reproduce the first value. With
        // the lag, this second 203 read returns the 100 result instead.
        TEST("ComputeChain_NoStaleLag", readOk && std::fabs(a1 - a2) < 1e-4f);

        // --- and the same thing ONE HOP FURTHER OUT --------------------------
        // The direct compute->compute case above passed while
        // compute -> PIXEL SHADER -> compute still lagged, because the middle
        // node is not in the deferred set even though its D2D output resolves
        // through a producer that has not been dispatched yet. Checking only
        // the immediate producer was not enough; the guard has to cover
        // everything DOWNSTREAM of this pass's dispatches. Pin both shapes so a
        // future narrowing of that set fails here rather than in a measurement.
        auto* midDesc = registry.FindByName(L"Nit Map");
        if (!midDesc) { TEST("ComputeChain2_FindDescriptor", false); return; }

        // m_effectCache is keyed on node id ALONE with no type check, and every
        // GraphModel starts its ids at 1 -- so without this the second graph
        // inherits the first graph's cached effects for the same ids and the
        // test may not be exercising the nodes it names.
        CaseEvaluator().ReleaseCache();
        ShaderLab::Graph::GraphModel g2;
        ShaderLab::Effects::SourceNodeFactory sf2;
        auto srcId2  = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto prodId2 = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*prodDesc));
        auto midId2  = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*midDesc));
        auto consId2 = g2.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*consDesc));
        g2.Connect(srcId2, 0, prodId2, 0);
        g2.Connect(prodId2, 0, midId2, 0);
        g2.Connect(midId2, 0, consId2, 0);
        ShaderLab::Graph::SetNodeProperty(*g2.FindNode(srcId2), L"Luminance", 400.0f);
        ShaderLab::Graph::SetNodeProperty(*g2.FindNode(midId2), L"Opacity", 0.0f);   // pass-through

        RenderFrame(g2, sf2);
        RenderFrame(g2, sf2);

        auto sweep2 = [&](float targetPeak) -> float {
            ShaderLab::Graph::SetNodeProperty(*g2.FindNode(prodId2), L"TargetPeakNits", targetPeak);
            RenderFrame(g2, sf2);
            auto r = ShaderLab::Rendering::ReadPixelRegion(g2, consId2, kProbeX, kProbeY, 1, 1, g_dc.get());
            return (r.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success &&
                    r.pixels.size() >= 4) ? r.pixels[1] : -1.0f;
        };
        const float c1 = sweep2(203.0f);
        const float c2 = sweep2(100.0f);
        const float c3 = sweep2(203.0f);
        const bool readOk2 = c1 >= 0.0f && c2 >= 0.0f && c3 >= 0.0f;
        TEST("ComputeChain2Hop_ReadbackSucceeded", readOk2);
        TEST("ComputeChain2Hop_TracksUpstream", readOk2 && std::fabs(c1 - c2) > 1e-3f);
        TEST("ComputeChain2Hop_NoStaleLag", readOk2 && std::fabs(c1 - c3) < 1e-4f);
    }

    // A 64x64 FP32 region of a node's output, or empty when unreadable.
    std::vector<float> ReadRegion(ShaderLab::Graph::GraphModel& graph, uint32_t nodeId, int32_t x, int32_t y)
    {
        auto region = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, x, y, 64, 64, g_dc.get());
        if (region.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success ||
            region.pixels.size() != 64 * 64 * 4)
            return {};
        return region.pixels;
    }

    // Largest difference relative to the value. ReadPixelRegion reads a
    // Direct2D node through an FP16 intermediate, while a compute node's
    // pre-render of it is FP32, so a match is within two FP16 roundings.
    float MaxRelativeDifference(const std::vector<float>& expected, const std::vector<float>& shown)
    {
        if (expected.empty() || expected.size() != shown.size()) return INFINITY;
        float largest = 0.0f;
        for (size_t i = 0; i < expected.size(); ++i)
            largest = (std::max)(largest,
                std::fabs(expected[i] - shown[i]) / (std::max)(std::fabs(expected[i]), 1.0f / 64.0f));
        return largest;
    }
    constexpr float cHalfPrecision = 1.0f / 512.0f;

    float ColorSum(const std::vector<float>& pixels)
    {
        float sum = 0.0f;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
        return sum;
    }

    // A compute node whose inputs come through other compute nodes and through
    // Direct2D chains sees this frame's inputs on the first rendered frame.
    SL_TEST_CASE(TestComputeFedByChains, Tag::Gpu)
    {
        printf("\n=== Compute node fed by compute and Direct2D chains ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        const auto* sourceDesc    = registry.FindByName(L"Gamut Source");
        const auto* histogramDesc = registry.FindByName(L"CIE Histogram");
        const auto* plotDesc      = registry.FindByName(L"CIE Chromaticity Plot");
        const auto* nitMapDesc    = registry.FindByName(L"Nit Map");
        const auto* toneMapDesc   = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        const auto* sideDesc      = registry.FindByName(L"Side by Side");
        if (!sourceDesc || !histogramDesc || !plotDesc || !nitMapDesc || !toneMapDesc || !sideDesc)
        {
            TEST("ComputeFedByChains_FindDescriptors", false);
            return;
        }
        using ShaderLab::Effects::ShaderLabEffects;
        constexpr float cSize = 256.0f;
        constexpr int32_t cProbe = 96;

        // Pins: source -> histogram -> plot; source -> Nit Map;
        // source -> tone map (compute) -> Nit Map; source. Every tile is
        // cSize square, so the row is native and each tile is its input at 1:1.
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            const auto sourceId    = g.AddNode(ShaderLabEffects::CreateNode(*sourceDesc));
            const auto histogramId = g.AddNode(ShaderLabEffects::CreateNode(*histogramDesc));
            const auto plotId      = g.AddNode(ShaderLabEffects::CreateNode(*plotDesc));
            const auto nitMapId    = g.AddNode(ShaderLabEffects::CreateNode(*nitMapDesc));
            const auto toneMapId   = g.AddNode(ShaderLabEffects::CreateNode(*toneMapDesc));
            const auto nitMap2Id   = g.AddNode(ShaderLabEffects::CreateNode(*nitMapDesc));
            const auto sideId      = g.AddNode(ShaderLabEffects::CreateNode(*sideDesc));
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(sourceId), L"OutputSize", cSize);
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(sourceId), L"Luminance", 400.0f);
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(histogramId), L"OutputSize", cSize);
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(plotId), L"DiagramSize", cSize);
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(sideId), L"Layout", 0.0f);
            g.Connect(sourceId, 0, histogramId, 0);
            g.Connect(histogramId, 0, plotId, 0);
            g.Connect(sourceId, 0, nitMapId, 0);
            g.Connect(sourceId, 0, toneMapId, 0);
            g.Connect(toneMapId, 0, nitMap2Id, 0);
            g.Connect(plotId, 0, sideId, 0);
            g.Connect(nitMapId, 0, sideId, 1);
            g.Connect(nitMap2Id, 0, sideId, 2);
            g.Connect(sourceId, 0, sideId, 3);

            for (auto& node : g.MutableNodes())
                if (node.type == ShaderLab::Graph::NodeType::Source)
                    sf.PrepareSourceNode(node, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            RunFrame(g);

            const auto* side = g.FindNode(sideId);
            TEST("ComputeFedByChains_FirstFrameHasOutput", side->cachedOutput != nullptr);
            TEST("ComputeFedByChains_NoDispatchError", side->runtimeError.empty());
            if (!side->runtimeError.empty())
                printf("      runtimeError: %ls\n", side->runtimeError.c_str());
            TEST("ComputeFedByChains_ConvergesInOneFrame", !g.HasDirtyNodes());

            // The histogram must actually hold counts under the probe, or a
            // plot without them would match a tile without them.
            const auto histogram = ReadRegion(g, histogramId, cProbe, cProbe);
            TEST("ComputeFedByChains_HistogramHasCounts", ColorSum(histogram) > 0.0f);

            const uint32_t sources[] = { plotId, nitMapId, nitMap2Id, sourceId };
            bool allMatch = true;
            for (int tile = 0; tile < 4; ++tile)
            {
                const auto expected = ReadRegion(g, sources[tile], cProbe, cProbe);
                const auto shown = ReadRegion(g, sideId, tile * static_cast<int32_t>(cSize) + cProbe, cProbe);
                const float difference = MaxRelativeDifference(expected, shown);
                printf("  [info] tile %d: max relative difference %g\n", tile, difference);
                if (!(difference < cHalfPrecision) || !(ColorSum(expected) > 0.0f))
                    allMatch = false;
            }
            TEST("ComputeFedByChains_EveryTileIsItsInput", allMatch);
        }

        // The same shape fed by a video, on the frame the video is first shown.
        const std::wstring videoPath = FindFixture(L"video_index_h264_180p30.mp4");
        if (videoPath.empty())
        {
            SkipMissingFixture(L"video_index_h264_180p30.mp4");
            return;
        }
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Graph::GraphModel g;
            ShaderLab::Effects::SourceNodeFactory sf;
            const auto videoId     = g.AddNode(ShaderLab::Effects::SourceNodeFactory::CreateVideoSourceNode(videoPath));
            const auto nitMapId    = g.AddNode(ShaderLabEffects::CreateNode(*nitMapDesc));
            const auto histogramId = g.AddNode(ShaderLabEffects::CreateNode(*histogramDesc));
            const auto plotId      = g.AddNode(ShaderLabEffects::CreateNode(*plotDesc));
            const auto sideId      = g.AddNode(ShaderLabEffects::CreateNode(*sideDesc));
            constexpr float cPlotSize = 128.0f;
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(histogramId), L"OutputSize", cPlotSize);
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(plotId), L"DiagramSize", cPlotSize);
            ShaderLab::Graph::SetNodeProperty(*g.FindNode(sideId), L"Layout", 2.0f);
            g.Connect(videoId, 0, nitMapId, 0);
            g.Connect(videoId, 0, histogramId, 0);
            g.Connect(histogramId, 0, plotId, 0);
            g.Connect(videoId, 0, sideId, 0);
            g.Connect(nitMapId, 0, sideId, 1);
            g.Connect(plotId, 0, sideId, 2);

            // Wait for the first decoded frame, as headless does before rendering.
            auto nodes = g.MutableNodes();
            auto* videoNode = g.FindNode(videoId);
            sf.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            auto* provider = sf.VideoProviderForRenderThread(videoId);
            for (int attempt = 0; provider && attempt < 300 && provider->UploadedFrameTime() < 0.0; ++attempt)
            {
                ::Sleep(10);
                sf.TickAndUploadVideos(nodes, g_dc.get());
            }
            if (!provider || provider->UploadedFrameTime() < 0.0)
            {
                TEST("ComputeFedByVideo_FrameDecoded", false);
                return;
            }
            sf.PrepareSourceNode(*videoNode, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            RunFrame(g);

            const auto* side = g.FindNode(sideId);
            TEST("ComputeFedByVideo_FirstFrameHasOutput", side->cachedOutput != nullptr && side->runtimeError.empty());

            // Grid of three 320x180 tiles; the 128 px plot sits centred in the third.
            constexpr int32_t cTileWidth = 320, cTileHeight = 180;
            constexpr int32_t cPlotLeft = (cTileWidth - 128) / 2, cPlotTop = (cTileHeight - 128) / 2;
            const float videoDifference = MaxRelativeDifference(ReadRegion(g, videoId, 100, 50),
                                                                ReadRegion(g, sideId, 100, 50));
            const float nitMapDifference = MaxRelativeDifference(ReadRegion(g, nitMapId, 100, 50),
                                                                 ReadRegion(g, sideId, cTileWidth + 100, 50));
            const float plotDifference = MaxRelativeDifference(ReadRegion(g, plotId, 32, 32),
                ReadRegion(g, sideId, cPlotLeft + 32, cTileHeight + cPlotTop + 32));
            printf("  [info] max relative difference: video %g, Nit Map %g, plot %g\n",
                   videoDifference, nitMapDifference, plotDifference);
            TEST("ComputeFedByVideo_VideoTileIsVideo", videoDifference < cHalfPrecision &&
                                                      ColorSum(ReadRegion(g, videoId, 100, 50)) > 0.0f);
            TEST("ComputeFedByVideo_ChainTileIsChain", nitMapDifference < cHalfPrecision);
            TEST("ComputeFedByVideo_PlotTileIsPlot", plotDifference < cHalfPrecision &&
                                                    ColorSum(ReadRegion(g, histogramId, 32, 32)) > 0.0f);
        }
    }

    // The reduction contract is written twice, as C++ constants and as HLSL
    // defines, and must agree or groups go missing silently.
    SL_TEST_CASE(TestReduceContractMatchesHlsl, Tag::Fast)
    {
        printf("\n=== Reduction contract: C++ vs HLSL ===\n");
        const std::string lib(ShaderLab::Effects::GetShaderLabParamsHLSL());
        auto defineValue = [&](const std::string& name) -> long long
        {
            const std::string key = "#define " + name + " ";
            auto p = lib.find(key);
            if (p == std::string::npos) return -1;
            return std::strtoll(lib.c_str() + p + key.size(), nullptr, 10);
        };
        TEST("Perf_ReduceGroupsMatchHlsl",
             defineValue("SHADERLAB_REDUCE_GROUPS") == ShaderLab::Effects::kReduceGroups);
        TEST("Perf_ReduceScratchMatchHlsl",
             defineValue("SHADERLAB_REDUCE_SCRATCH_UINTS") == ShaderLab::Effects::kReduceScratchUints);
        TEST("Perf_ReduceClearedMatchHlsl",
             defineValue("SHADERLAB_REDUCE_CLEARED") == ShaderLab::Effects::kReduceScratchClearedUints);
    }

    // Multi-group statistics count every pixel exactly once, including an
    // image wider than one group's 1024 threads and with fewer rows than
    // there are groups (most groups get no rows at all).
    SL_TEST_CASE(TestMultiGroupStatistics, Tag::Gpu)
    {
        printf("\n=== Multi-group statistics ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto& d2dReg   = ShaderLab::Effects::EffectRegistry::Instance();
        ShaderLab::Graph::GraphModel g;
        auto gradId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gradient Generator")));
        auto cropId = g.AddNode(d2dReg.CreateNode(*d2dReg.FindByName(L"Crop")));
        auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        ShaderLab::Graph::SetNodeProperty(*g.FindNode(gradId), L"GradSize", 2048.0f);
        g.FindNode(cropId)->properties[L"Rect"] =
            winrt::Windows::Foundation::Numerics::float4{ 0.0f, 0.0f, 2048.0f, 37.0f };
        g.Connect(gradId, 0, cropId, 0);
        g.Connect(cropId, 0, statsId, 0);
        g.MarkAllDirty();
        RunFrame(g);
        g.MarkAllDirty();
        RunFrame(g);
        auto* sn = g.FindNode(statsId);
        const float samples = AnalysisField(sn, L"Samples");
        const float mx = AnalysisField(sn, L"Max");
        const float mean = AnalysisField(sn, L"Mean");
        TEST("Perf_MultiGroupStatsCountEveryPixelOnce", samples == 2048.0f * 37.0f);
        TEST("Perf_MultiGroupStatsMaxIsGradientEnd", mx > 79.0f && mx <= 80.01f);
        TEST("Perf_MultiGroupStatsMeanIsGradientMidpoint", std::abs(mean - 40.0f) < 0.5f);
        if (samples != 2048.0f * 37.0f || std::abs(mean - 40.0f) >= 0.5f)
            printf("    (debug) Samples=%.0f Max=%.4f Mean=%.4f\n", samples, mx, mean);
    }

    // An unbounded input (Flood) into a compute node is refused with a
    // message, instead of an 8192^2 FP32 (1 GiB) pre-render.
    SL_TEST_CASE(TestUnboundedComputeInputRefused, Tag::Gpu)
    {
        printf("\n=== Unbounded compute input ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Effects::SourceNodeFactory sf;
        ShaderLab::Graph::GraphModel g;
        auto floodId = g.AddNode(ShaderLab::Effects::SourceNodeFactory::CreateFloodSourceNode(
            winrt::Windows::Foundation::Numerics::float4{ 1.0f, 1.0f, 1.0f, 1.0f }));
        auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        g.Connect(floodId, 0, statsId, 0);
        for (auto& n : g.MutableNodes())
            if (n.type == ShaderLab::Graph::NodeType::Source)
                sf.PrepareSourceNode(n, g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
        g.MarkAllDirty();
        RunFrame(g);
        auto* sn = g.FindNode(statsId);
        TEST("Perf_UnboundedComputeInputIsRefused",
             sn && sn->runtimeError.find(L"unbounded") != std::wstring::npos);
    }

    // A compute consumer bound to a compute producer dispatches once per
    // cycle even when the host evaluates twice: the queue flag stays set
    // until ProcessDeferredCompute, so a second pass must not queue the
    // consumer again. Evaluates twice unconditionally on purpose, which
    // FrameExecutor never does.
    SL_TEST_CASE(TestBoundConsumerDispatchedOnce, Tag::Gpu)
    {
        printf("\n=== Bound compute consumer dispatched once ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Performance::SetGpuBindingsEnabled(true);
        ShaderLab::Graph::GraphModel g;
        auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Gamut Source")));
        auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        auto tmId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)")));
        g.Connect(srcId, 0, statsId, 0);
        g.Connect(srcId, 0, tmId, 0);
        g.BindProperty(tmId, L"SourcePeakNits", statsId, L"Max", 0);
        for (int i = 0; i < 3; ++i) { g.MarkAllDirty(); RunFrame(g); }

        g.MarkAllDirty();
        g_dc->SetTarget(nullptr);
        CaseEvaluator().Evaluate(g, g_dc.get());
        CaseEvaluator().Evaluate(g, g_dc.get());
        CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
        TEST("Perf_BoundComputeConsumerDispatchedOnce", CaseEvaluator().DispatchesLastFrame() == 2);
        if (CaseEvaluator().DispatchesLastFrame() != 2)
            printf("    (debug) dispatches=%u\n", CaseEvaluator().DispatchesLastFrame());
    }

    // The graph primitives dirty exactly what they change, and a change made
    // while a node is not needed survives until it is.
    SL_TEST_CASE(TestTargetedDirtying, Tag::Gpu)
    {
        printf("\n=== Targeted dirtying ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Graph::GraphModel g;
        auto mk = [&](const wchar_t* n) {
            return g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(n)));
        };
        auto a = mk(L"Gamut Source"), b = mk(L"Gamut Source");
        auto sa = mk(L"Luminance Statistics"), sb = mk(L"Luminance Statistics");
        auto tm = mk(L"ICtCp Round-Trip Validator");
        g.Connect(a, 0, sa, 0);
        g.Connect(b, 0, sb, 0);
        g.Connect(a, 0, tm, 0);
        auto clean = [&] { for (auto& n : g.MutableNodes()) n.dirty = false; };
        auto dirty = [&](uint32_t id) { return g.FindNode(id)->dirty; };

        clean();
        g.BindProperty(tm, L"Gain", sa, L"Max", 0);
        TEST("Dirty_BindMarksOnlyConsumer", dirty(tm) && !dirty(a) && !dirty(b) && !dirty(sa) && !dirty(sb));

        clean();
        g.RemoveNode(sa);   // consumed by tm through the binding
        TEST("Dirty_RemoveNodeDirtiesBindingConsumer", dirty(tm) && !dirty(b) && !dirty(sb));

        clean();
        g.RemoveNode(a);    // consumed by tm through an edge
        TEST("Dirty_RemoveNodeDirtiesEdgeConsumer", dirty(tm) && !dirty(b) && !dirty(sb));

        clean();
        g.FindNode(sb)->needed = false;
        ShaderLab::Graph::MarkNodeChanged(*g.FindNode(sb));
        TEST("Dirty_UnneededDoesNotCountAsDirty", !g.HasDirtyNodes());
        g_dc->SetTarget(nullptr);
        CaseEvaluator().Evaluate(g, g_dc.get());
        TEST("Dirty_UnneededKeepsPendingChange", dirty(sb));
        g.FindNode(sb)->needed = true;
        TEST("Dirty_PendingChangeSurfacesWhenNeeded", g.HasDirtyNodes());
    }

    // Async readback: the values land a frame or two later, equal to what a
    // blocking read returns, and the evaluator reports the pending copy so a
    // host keeps rendering until it does.
    SL_TEST_CASE(TestAsyncReadback, Tag::Gpu)
    {
        printf("\n=== Async analysis readback ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto build = [&](ShaderLab::Graph::GraphModel& g) -> uint32_t
        {
            auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source")));
            auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            g.Connect(srcId, 0, statsId, 0);
            return statsId;
        };

        ShaderLab::Graph::GraphModel gb;
        auto sb = build(gb);
        for (int i = 0; i < 2; ++i) { gb.MarkAllDirty(); RunFrame(gb); }
        const float blockingMax = AnalysisField(gb.FindNode(sb), L"Max");

        // The second graph reuses node ids 1 and 2.
        CaseEvaluator().ReleaseCache();
        ShaderLab::Graph::GraphModel ga;
        auto sa = build(ga);
        ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
        ShaderLab::Performance::SetAsyncAnalysisReadbackEnabled(true);
        CaseEvaluator().SetCpuAnalysisInterest({ sa });
        for (int i = 0; i < 2; ++i) { ga.MarkAllDirty(); RunFrame(ga); }
        // Let pending copies land: frames with nothing dirty still poll.
        bool sawPending = CaseEvaluator().HasPendingReadbacks();
        for (int i = 0; i < 200 && CaseEvaluator().HasPendingReadbacks(); ++i)
        {
            ::Sleep(1);
            RunFrame(ga);
        }
        const float asyncMax = AnalysisField(ga.FindNode(sa), L"Max");
        TEST("Perf_AsyncReadbackWasQueued", sawPending);
        TEST("Perf_AsyncReadbackDrains", !CaseEvaluator().HasPendingReadbacks());
        TEST("Perf_AsyncReadbackMatchesBlocking",
             blockingMax > 0.0f && asyncMax == blockingMax);
        if (asyncMax != blockingMax)
            printf("    (debug) blocking Max=%.4f async Max=%.4f\n", blockingMax, asyncMax);
    }

    // Frames wider than 4096 px (a multi-monitor desktop) reach the far
    // columns through custom pixel shaders and compute nodes. An unbounded
    // input still gets a finite default rect, and a compute input past the
    // D3D11 texture limit is refused with a message.
    SL_TEST_CASE(TestWideFrames, Tag::Gpu)
    {
        printf("\n=== Wide frames through pixel shaders and compute ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        // Exact in FP16: red = column / 128 in steps of 1/64, green = column
        // within that step, blue = row.
        const auto pattern = PatternEffect(L"Wide Test Pattern",
            "    return float4(floor(pixel.x / 128.0) / 64.0, fmod(pixel.x, 128.0) / 128.0,\n"
            "                  (pixel.y + 1.0) / 4096.0, input.a);\n");
        const auto doubler = PatternEffect(L"Wide Test Doubler",
            "    return float4(input.rgb * 2.0, input.a);\n");
        auto expected = [](int x, int y) -> std::array<float, 3>
        {
            return { 2.0f * std::floor(x / 128.0f) / 64.0f, 2.0f * static_cast<float>(x % 128) / 128.0f,
                     2.0f * (y + 1.0f) / 4096.0f };
        };
        auto columnsMatch = [&](ShaderLab::Graph::GraphModel& graph, uint32_t nodeId, int x, int y) -> bool
        {
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, x, y, 4, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 16)
            {
                printf("    (debug) readback at x=%d failed\n", x);
                return false;
            }
            for (int i = 0; i < 4; ++i)
            {
                const auto want = expected(x + i, y);
                for (int channel = 0; channel < 3; ++channel)
                {
                    if (std::abs(readback.pixels[i * 4 + channel] - want[channel]) > 1e-3f)
                    {
                        printf("    (debug) x=%d channel %d: %.6f, expected %.6f\n",
                            x + i, channel, readback.pixels[i * 4 + channel], want[channel]);
                        return false;
                    }
                }
            }
            return true;
        };

        // 7680 wide: two 4K monitors side by side, kept short for test time.
        {
            CaseEvaluator().ReleaseCache();
            constexpr int cWidth = 7680;
            constexpr int cHeight = 128;
            ShaderLab::Effects::SourceNodeFactory sourceFactory;
            ShaderLab::Graph::GraphModel graph;
            const auto patternId = AddPatternSource(graph, sourceFactory, pattern, cWidth, cHeight);
            const auto doublerId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(doubler));
            const auto statsId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            graph.Connect(patternId, 0, doublerId, 0);
            graph.Connect(doublerId, 0, statsId, 0);
            graph.MarkAllDirty();
            RunFrame(graph);
            graph.MarkAllDirty();
            RunFrame(graph);

            D2D1_RECT_F bounds{};
            if (const auto* doublerNode = graph.FindNode(doublerId); doublerNode && doublerNode->cachedOutput)
                g_dc->GetImageLocalBounds(doublerNode->cachedOutput, &bounds);
            TEST("Wide_PixelShaderKeepsFullWidth", bounds.right == cWidth && bounds.bottom == cHeight);
            TEST("Wide_LeftColumnsCorrect", columnsMatch(graph, doublerId, 0, 5));
            TEST("Wide_MiddleColumnsCorrect", columnsMatch(graph, doublerId, 4094, 77));
            TEST("Wide_FarRightColumnsCorrect", columnsMatch(graph, doublerId, cWidth - 4, cHeight - 1));

            double sum = 0.0;
            double maxNits = 0.0;
            for (int y = 0; y < cHeight; ++y)
            {
                for (int x = 0; x < cWidth; ++x)
                {
                    const auto rgb = expected(x, y);
                    const double nits = (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]) * 80.0;
                    sum += nits;
                    maxNits = (std::max)(maxNits, nits);
                }
            }
            const double cpuMean = sum / (static_cast<double>(cWidth) * cHeight);
            const auto* statsNode = graph.FindNode(statsId);
            const float samples = AnalysisField(statsNode, L"Samples");
            const float mean = AnalysisField(statsNode, L"Mean");
            const float maxValue = AnalysisField(statsNode, L"Max");
            printf("  [info] 7680 wide stats: Samples %.0f Mean %.4f (CPU %.4f) Max %.4f (CPU %.4f)\n",
                samples, mean, cpuMean, maxValue, maxNits);
            TEST("Wide_StatisticsSeeEveryColumn", samples == static_cast<float>(cWidth * cHeight));
            TEST("Wide_StatisticsMeanMatchesCpu", std::abs(mean - cpuMean) <= 1e-3 * cpuMean);
            TEST("Wide_StatisticsMaxMatchesCpu", std::abs(maxValue - maxNits) <= 1e-3 * maxNits);
        }

        // Past the D3D11 texture limit: the pixel shader still renders (D2D
        // tiles it), and a compute node says why it cannot read the input.
        {
            CaseEvaluator().ReleaseCache();
            constexpr int cWidth = 16400;
            constexpr int cHeight = 8;
            ShaderLab::Effects::SourceNodeFactory sourceFactory;
            ShaderLab::Graph::GraphModel graph;
            const auto patternId = AddPatternSource(graph, sourceFactory, pattern, cWidth, cHeight);
            const auto doublerId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(doubler));
            const auto statsId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            graph.Connect(patternId, 0, doublerId, 0);
            graph.Connect(doublerId, 0, statsId, 0);
            graph.MarkAllDirty();
            RunFrame(graph);
            graph.MarkAllDirty();
            RunFrame(graph);
            TEST("Wide_PastTextureLimitPixelShaderRenders", columnsMatch(graph, doublerId, cWidth - 4, cHeight - 1));
            const auto* statsNode = graph.FindNode(statsId);
            TEST("Wide_PastTextureLimitComputeRefused",
                 statsNode && statsNode->runtimeError.find(L"16384") != std::wstring::npos);
        }

        // An unbounded input still yields the default 4096 x 4096 rect.
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Effects::SourceNodeFactory sourceFactory;
            ShaderLab::Graph::GraphModel graph;
            const auto floodId = graph.AddNode(ShaderLab::Effects::SourceNodeFactory::CreateFloodSourceNode(
                winrt::Windows::Foundation::Numerics::float4{ 1.0f, 1.0f, 1.0f, 1.0f }));
            const auto patternId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(pattern));
            graph.Connect(floodId, 0, patternId, 0);
            sourceFactory.PrepareSourceNode(*graph.FindNode(floodId), g_dc.get(), g_d3dDevice.get(), g_d3dContext.get());
            graph.MarkAllDirty();
            RunFrame(graph);
            graph.MarkAllDirty();
            RunFrame(graph);
            D2D1_RECT_F bounds{};
            if (const auto* patternNode = graph.FindNode(patternId); patternNode && patternNode->cachedOutput)
                g_dc->GetImageLocalBounds(patternNode->cachedOutput, &bounds);
            printf("  [info] unbounded input -> (%.0f, %.0f, %.0f, %.0f)\n", bounds.left, bounds.top, bounds.right, bounds.bottom);
            TEST("Wide_UnboundedInputGetsDefaultRect",
                 bounds.left == 0.0f && bounds.top == 0.0f && bounds.right == 4096.0f && bounds.bottom == 4096.0f);
        }
        CaseEvaluator().ReleaseCache();
    }

    // A compute node reads a float bitmap's own texture instead of an FP32
    // copy, and still sees new contents written into that texture.
    SL_TEST_CASE(TestComputeReadsTexturesInPlace, Tag::Gpu)
    {
        printf("\n=== Compute reads textures in place ===\n");
        constexpr UINT cWidth = 64, cHeight = 32;
        auto toHalf = [](float f) -> uint16_t
        {
            uint32_t b; std::memcpy(&b, &f, 4);
            return static_cast<uint16_t>(((b >> 16) & 0x8000) | ((((b >> 23) & 0xFF) - 112) << 10) | ((b >> 13) & 0x3FF));
        };
        // Grey: left half `left`, right half `right`, alpha 1.
        auto fill = [&](float left, float right)
        {
            std::vector<uint16_t> texels(cWidth * cHeight * 4);
            for (UINT y = 0; y < cHeight; ++y)
                for (UINT x = 0; x < cWidth; ++x)
                {
                    const uint16_t v = toHalf(x < cWidth / 2 ? left : right);
                    uint16_t* p = &texels[(y * cWidth + x) * 4];
                    p[0] = p[1] = p[2] = v;
                    p[3] = toHalf(1.0f);
                }
            return texels;
        };

        D3D11_TEXTURE2D_DESC td{};
        td.Width = cWidth; td.Height = cHeight; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        auto texels = fill(1.0f, 3.0f);
        D3D11_SUBRESOURCE_DATA init{ texels.data(), cWidth * 8, 0 };
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(g_d3dDevice->CreateTexture2D(&td, &init, texture.put()));

        auto wrap = [&](D2D1_ALPHA_MODE alpha)
        {
            D2D1_BITMAP_PROPERTIES1 props{};
            props.pixelFormat = { DXGI_FORMAT_R16G16B16A16_FLOAT, alpha };
            props.dpiX = props.dpiY = 96.0f;
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            g_dc->CreateBitmapFromDxgiSurface(texture.as<IDXGISurface>().get(), &props, bitmap.put());
            return bitmap;
        };
        auto bitmap = wrap(D2D1_ALPHA_MODE_PREMULTIPLIED);
        const D2D1_RECT_L whole{ 0, 0, cWidth, cHeight };
        TEST("InPlace_Fp16BitmapReadsItsTexture",
             ShaderLab::Rendering::ComputeReadableTexture(bitmap.get(), whole).get() == texture.get());
        TEST("InPlace_OffsetRectIsCopied",
             !ShaderLab::Rendering::ComputeReadableTexture(bitmap.get(), D2D1_RECT_L{ 1, 0, cWidth, cHeight }));
        auto ignoreAlpha = wrap(D2D1_ALPHA_MODE_IGNORE);
        TEST("InPlace_IgnoredAlphaIsCopied",
             ignoreAlpha && !ShaderLab::Rendering::ComputeReadableTexture(ignoreAlpha.get(), whole));

        // A Source node fed the bitmap directly, into Luminance Statistics.
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Graph::GraphModel g;
        const auto sourceId = g.AddNode(ShaderLab::Effects::SourceNodeFactory::CreateImageSourceNode(L"", L"Texture"));
        const auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        g.Connect(sourceId, 0, statsId, 0);
        g.FindNode(sourceId)->cachedOutput = bitmap.get();
        g.MarkAllDirty();
        RunFrame(g);
        const auto* stats = g.FindNode(statsId);
        TEST("InPlace_StatsMatchTexture",
             AnalysisField(stats, L"Samples") == cWidth * cHeight &&
             std::abs(AnalysisField(stats, L"Max") - 240.0f) < 0.05f &&
             std::abs(AnalysisField(stats, L"Mean") - 160.0f) < 0.05f);

        // New contents in the same texture: the next frame measures them.
        texels = fill(2.0f, 6.0f);
        g_d3dContext->UpdateSubresource(texture.get(), 0, nullptr, texels.data(), cWidth * 8, 0);
        ShaderLab::Graph::MarkNodeChanged(*g.FindNode(sourceId));
        RunFrame(g);
        stats = g.FindNode(statsId);
        TEST("InPlace_StatsFollowTextureWrites",
             std::abs(AnalysisField(stats, L"Max") - 480.0f) < 0.1f &&
             std::abs(AnalysisField(stats, L"Mean") - 320.0f) < 0.1f);
        if (std::abs(AnalysisField(stats, L"Max") - 480.0f) >= 0.1f)
            printf("    (debug) Max=%.4f Mean=%.4f\n", AnalysisField(stats, L"Max"), AnalysisField(stats, L"Mean"));
        g.FindNode(sourceId)->cachedOutput = nullptr;
        CaseEvaluator().ReleaseCache();
    }

    // The derived-constant memo behind a built-in is shared by every
    // evaluator in the process; concurrent callers must each get their own
    // inputs' table.
    SL_TEST_CASE(TestDerivedConstantsConcurrent, Tag::Fast)
    {
        printf("\n=== Derived constants from several threads ===\n");
        const auto* descriptor = ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(L"ICtCp Boundary");
        if (!descriptor || !descriptor->deriveConstants)
        {
            TEST("DerivedConcurrent_DescriptorFound", false);
            return;
        }
        auto derive = [&](float intensity) -> std::vector<float>
        {
            std::map<std::wstring, ShaderLab::Graph::PropertyValue> props;
            props[L"Intensity"] = intensity;
            std::vector<float> out;
            descriptor->deriveConstants(props, [&](std::wstring_view, const float* data, size_t count)
            {
                out.assign(data, data + count);
            });
            return out;
        };

        constexpr int cIntensities = 24;   // more than the memo holds, so it clears under load
        std::vector<std::vector<float>> expected(cIntensities);
        for (int i = 0; i < cIntensities; ++i)
            expected[i] = derive(0.1f + 0.03f * i);

        std::atomic<int> mismatches{ 0 };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
        {
            threads.emplace_back([&, t]
            {
                for (int n = 0; n < 200; ++n)
                {
                    const int i = (n * 7 + t * 5) % cIntensities;
                    if (derive(0.1f + 0.03f * i) != expected[i]) ++mismatches;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        TEST("DerivedConcurrent_ReferenceNonEmpty", !expected[0].empty());
        TEST("DerivedConcurrent_EveryCallGotItsTable", mismatches.load() == 0);
    }
}
