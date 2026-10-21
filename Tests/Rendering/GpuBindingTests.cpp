#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/Performance.h"
#include "Effects/ShaderCompiler.h"
#include "Effects/ShaderVariants.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/PixelReadback.h"
#include "Rendering/SourceRuntime.h"
#include "Graph/GraphEdits.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// Analysis values reaching consumers: GPU bindings (lane 3), CPU readback
// policy and per-evaluator options.
namespace
{
    using namespace ShaderLab::Tests;

    // GPU-binding routing through the compute bridge: Gamut Source -> Luminance
    // Statistics -> ICtCp Tone Map with TargetPeakNits bound to Mean. The tone
    // map renders with GPU bindings on and off, and with them on but no
    // upstream binding.
    SL_TEST_CASE(TestGpuBindingRouting, Tag::Gpu)
    {
        printf("\n=== GPU-binding routing ===\n");

        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* ictcpDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        if (!srcDesc || !statsDesc || !ictcpDesc) {
            TEST("GpuBinding_DescriptorsFound", false);
            return;
        }
        TEST("GpuBinding_ICtCpIsD3D11Compute",
             ictcpDesc->shaderType == ShaderLab::Graph::CustomShaderType::D3D11ComputeShader);

        // ---- Sub-test 1: Feature flag ON, with binding ----
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::GraphModel g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto statsId = g.AddNode(std::move(statsNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));

            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);

            // Bind ICtCp's TargetPeakNits to Luminance Statistics' Mean.
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);

            PrepareSourcesAndEvaluate(g, sf);
            g_dc->SetTarget(nullptr);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            // Run twice so the first-frame just-created delay clears.
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

            // TargetPeakNits is the tone mapper's first gpu-bindable parameter.
            const auto targetPeakIndex = [&]
            {
                uint32_t index = 0;
                for (const auto& param : g.FindNode(ictcpId)->customEffect->parameters)
                {
                    if (!param.gpuBindable) continue;
                    if (param.name == L"TargetPeakNits") return index;
                    ++index;
                }
                return UINT32_MAX;
            }();
            TEST("GpuBinding_ConsumerRanStructuredBufferBuild",
                 ShaderLab::Effects::BindingModeAt(CaseEvaluator().AppliedGpuBits(ictcpId), targetPeakIndex) ==
                     ShaderLab::Effects::GpuBindingMode::StructuredBuffer);

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOn", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOn", n && n->runtimeError.empty());
        }

        // ---- Sub-test 2: Feature flag OFF, with binding ----
        // CPU readback path. Should still work.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::GraphModel g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto statsId = g.AddNode(std::move(statsNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);

            ShaderLab::Graph::PropertyBinding pb;
            ShaderLab::Graph::ComponentSource cs;
            cs.sourceNodeId    = statsId;
            cs.sourceFieldName = L"Mean";
            cs.sourceComponent = 0;
            pb.sources.push_back(cs);
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);

            ShaderLab::Performance::SetGpuBindingsEnabled(false);
            CaseEvaluator().ReleaseCache();
            PrepareSourcesAndEvaluate(g, sf);
            g_dc->SetTarget(nullptr);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOff", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOff", n && n->runtimeError.empty());
        }

        // ---- Sub-test 3: Feature flag ON, NO binding ----
        // Baseline (cbuffer mode) path even with flag on.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::GraphModel g;
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            auto srcId   = g.AddNode(std::move(srcNode));
            auto ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, ictcpId, 0);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            CaseEvaluator().ReleaseCache();
            PrepareSourcesAndEvaluate(g, sf);
            g_dc->SetTarget(nullptr);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

            auto* n = g.FindNode(ictcpId);
            TEST("GpuBinding_ICtCpHasOutput_FlagOnNoBinding", n && n->cachedOutput != nullptr);
            TEST("GpuBinding_ICtCpNoRuntimeError_FlagOnNoBinding", n && n->runtimeError.empty());
        }
    }

    // A node bound to an analysis node sees that node's new values in the same
    // frame. Nothing reaches it by edge (Luminance Statistics has no image
    // output), so the binding is the only path.
    SL_TEST_CASE(TestBoundConsumerFreshSameFrame, Tag::Gpu)
    {
        printf("\n=== Bound consumer sees analysis values the same frame ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* tmDesc    = registry.FindByName(L"ICtCp Round-Trip Validator");
        if (!srcDesc || !statsDesc || !tmDesc) { TEST("BoundFresh_DescriptorsFound", false); return; }

        ShaderLab::Effects::SourceNodeFactory sf;
        ShaderLab::Graph::GraphModel g;
        auto srcId   = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc));
        auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc));
        auto tmId    = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*tmDesc));
        g.Connect(srcId, 0, statsId, 0);
        g.Connect(srcId, 0, tmId, 0);
        g.BindProperty(tmId, L"Gain", statsId, L"Max", 0);
        ShaderLab::Performance::SetGpuBindingsEnabled(false);   // CPU path: the value lands in properties

        // ONE frame, as every host renders it.
        RenderFrame(g, sf);

        const float statsMax = AnalysisField(g.FindNode(statsId), L"Max");
        float tmPeak = -1.0f;
        if (auto* tn = g.FindNode(tmId))
            if (auto it = tn->properties.find(L"Gain"); it != tn->properties.end())
                if (auto* f = std::get_if<float>(&it->second)) tmPeak = *f;

        // Instrument check first: the measured Max must differ from the
        // parameter's default (1000), or "equal" below would prove nothing.
        TEST("BoundFresh_StatsProducedAValue", statsMax > 0.0f && std::abs(statsMax - 1000.0f) > 1.0f);
        TEST("BoundFresh_ConsumerHasThisFramesValue",
             statsMax > 0.0f && std::abs(tmPeak - statsMax) <= 1e-3f * statsMax);
    }

    // Lane 3, consumer side, end to end: an analysis value reaching a PIXEL
    // shader as a GPU texture. No public built-in pixel shader has a
    // gpu-bindable parameter, so this defines a tiny one here. It also
    // declares a lookup input, pinning the reserved-pin layout the evaluator
    // and the shader must agree on:
    //     pin 0  Source (image)   pin 1  Table (lookup)   pin 2  Level (gpu-bindable)
    // The shader outputs Level in every channel, so the pixel IS the bound value.
    SL_TEST_CASE(TestLane3PixelConsumer, Tag::Gpu)
    {
        printf("\n=== Lane 3: analysis value into a pixel shader as a texture ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Effects::ShaderLabEffectDescriptor d;
        d.name = L"Lane3 Test Consumer";
        d.effectId = L"Lane3 Test Consumer"; d.effectVersion = 1;
        d.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        d.inputNames = { L"Source", L"Table" };
        d.lookupInputCount = 1;
        d.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
Texture2D Table  : register(t1);
SHADERLAB_GPU_BUFFER(Level, t2)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0, float4 uv1 : TEXCOORD1, float4 uv2 : TEXCOORD2) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    float a = Source.Sample(InputSampler, uv0.xy).a;
    return float4(Level, Level, Level, a);
}
)HLSL";
        d.parameters = {
            ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", true },
        };

        ScopedPerformanceSettings settings;
        uint32_t appliedBits = 0;
        auto run = [&](bool gpu) -> std::pair<float, float>
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Performance::SetGpuBindingsEnabled(gpu);
            ShaderLab::Graph::GraphModel g;
            auto src   = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
            auto stats = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Luminance Statistics")));
            auto cons  = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(d));
            g.Connect(src, 0, stats, 0);
            g.Connect(src, 0, cons, 0);             // Table left unwired: the placeholder fills it
            g.BindProperty(cons, L"Level", stats, L"Max", 0);
            // Texture mode engages only once the producer has published its
            // texture, so give it a few frames, as a live graph would.
            for (int i = 0; i < 4; ++i)
            {
                g.MarkAllDirty();
                RunFrame(g);
            }
            const float maxNits = AnalysisField(g.FindNode(stats), L"Max");
            auto r = ShaderLab::Rendering::ReadPixelRegion(g, cons, 8, 8, 1, 1, g_dc.get());
            const float px = (r.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success && r.pixels.size() >= 4)
                ? r.pixels[0] : -1.0f;
            auto* cn = g.FindNode(cons);
            if (!cn->runtimeError.empty()) printf("    (debug) consumer error: %ls\n", cn->runtimeError.c_str());
            appliedBits = CaseEvaluator().AppliedGpuBits(cons);
            return { px, maxNits };
        };

        auto [gpuPx, gpuMax] = run(true);
        const uint32_t gpuAppliedBits = appliedBits;
        auto [cpuPx, cpuMax] = run(false);
        CaseEvaluator().ReleaseCache();

        printf("  [info] Max=%.4f  pixel via texture=%.4f  via cbuffer=%.4f\n", gpuMax, gpuPx, cpuPx);
        TEST("Lane3Consumer_ProducerMeasuredSomething", gpuMax > 1.0f);
        TEST("Lane3Consumer_GpuBindingWasUsed", gpuAppliedBits == 2u && appliedBits == 0u);
        TEST("Lane3Consumer_TextureDeliversTheValue", std::abs(gpuPx - gpuMax) <= 1e-3f * gpuMax);
        TEST("Lane3Consumer_CbufferPathAgrees", std::abs(cpuPx - cpuMax) <= 1e-3f * cpuMax && std::abs(cpuPx - gpuPx) <= 1e-3f * gpuMax);
    }

    // Lane 3 on a static graph: nothing is dirtied after it is built, so the
    // producer dispatches only on the first frame, and that dispatch has to
    // publish the texture. Its request comes before the bridge initializes its
    // runner, which once dropped it and left the consumer on the cbuffer.
    SL_TEST_CASE(TestLane3StaticGraph, Tag::Gpu)
    {
        printf("\n=== Lane 3 on a graph that never changes ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Effects::ShaderLabEffectDescriptor consumer;
        consumer.name = L"Lane3 Static Consumer";
        consumer.effectId = L"Lane3 Static Consumer"; consumer.effectVersion = 1;
        consumer.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        consumer.inputNames = { L"Source" };
        consumer.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Level, t1)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    return float4(Level, Level, Level, Source.Sample(InputSampler, uv0.xy).a);
}
)HLSL";
        consumer.parameters = {
            ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", true },
        };

        // Its own evaluator, so no earlier case's runners are already initialized.
        ShaderLab::Rendering::GraphEvaluator evaluator;
        auto options = ShaderLab::Performance::DefaultEvaluatorOptions();
        options.gpuBindings = true;
        evaluator.SetOptions(options);

        ShaderLab::Graph::GraphModel g;
        const auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
        const auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        const auto consId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(consumer));
        g.Connect(srcId, 0, statsId, 0);
        g.Connect(srcId, 0, consId, 0);
        g.BindProperty(consId, L"Level", statsId, L"Max", 0);

        uint32_t bitsPerFrame[4]{};
        for (auto& bits : bitsPerFrame)
        {
            ShaderLab::Rendering::FrameExecutor::Run(g, evaluator, g_dc.get(), {});
            bits = evaluator.AppliedGpuBits(consId);
        }
        const float maxNits = AnalysisField(g.FindNode(statsId), L"Max");
        auto r = ShaderLab::Rendering::ReadPixelRegion(g, consId, 8, 8, 1, 1, g_dc.get());
        const float px = (r.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success && r.pixels.size() >= 4)
            ? r.pixels[0] : -1.0f;
        printf("  [info] consumer bits per frame: %u %u %u %u; Max=%.4f pixel=%.4f\n",
               bitsPerFrame[0], bitsPerFrame[1], bitsPerFrame[2], bitsPerFrame[3], maxNits, px);

        TEST("Lane3Static_ProducerMeasuredSomething", maxNits > 1.0f);
        TEST("Lane3Static_TextureServedFromTheFirstFrame", bitsPerFrame[0] == 2u);
        TEST("Lane3Static_TextureServedWithoutInvalidation", bitsPerFrame[3] == 2u);
        TEST("Lane3Static_TextureDeliversTheValue", maxNits > 1.0f && std::abs(px - maxNits) <= 1e-3f * maxNits);
        evaluator.ReleaseCache(g);
    }

    // With readbacks skipped, a producer whose only consumer reads it on the
    // GPU never copies its values to the CPU, the first frame included: the
    // consumer's build is chosen after the dispatch, so the plan judges it by
    // the build it will get rather than the one it last ran. A CPU consumer
    // bound later reads the GPU results without a redispatch.
    SL_TEST_CASE(TestLane3NoCpuReadback, Tag::Gpu)
    {
        printf("\n=== Lane 3 without a CPU readback ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        auto consumerOf = [](const wchar_t* name, bool gpuBindable)
        {
            ShaderLab::Effects::ShaderLabEffectDescriptor d;
            d.name = name;
            d.effectId = name; d.effectVersion = 1;
            d.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
            d.inputNames = { L"Source" };
            d.hlslSource = gpuBindable ? R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Level, t1)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    return float4(Level, Level, Level, Source.Sample(InputSampler, uv0.xy).a);
}
)HLSL" : R"HLSL(
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    float Level;
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0) : SV_TARGET
{
    return float4(Level, Level, Level, Source.Sample(InputSampler, uv0.xy).a);
}
)HLSL";
            d.parameters = {
                ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", gpuBindable },
            };
            return d;
        };
        const auto gpuConsumer = consumerOf(L"Lane3 Readback Gpu Consumer", true);
        const auto cpuConsumer = consumerOf(L"Lane3 Readback Cpu Consumer", false);

        ShaderLab::Rendering::GraphEvaluator evaluator;
        auto options = ShaderLab::Performance::DefaultEvaluatorOptions();
        options.gpuBindings = true;
        options.skipUnneededCpuReadback = true;
        options.asyncAnalysisReadback = false;
        evaluator.SetOptions(options);
        evaluator.ClearCpuAnalysisInterest();

        ShaderLab::Graph::GraphModel g;
        auto gradient = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gradient Generator"));
        ShaderLab::Graph::SetNodeProperty(gradient, L"GradSize", 64.0f);
        const auto gradientId = g.AddNode(std::move(gradient));
        const auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        const auto gpuId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(gpuConsumer));
        g.Connect(gradientId, 0, statsId, 0);
        g.Connect(gradientId, 0, gpuId, 0);
        g.BindProperty(gpuId, L"Level", statsId, L"Max", 0);

        auto setLevel = [&](float value)
        {
            auto* node = g.FindNode(gradientId);
            for (const wchar_t* key : { L"StartR", L"StartG", L"StartB", L"EndR", L"EndG", L"EndB" })
                ShaderLab::Graph::SetNodeProperty(*node, key, value);
            ShaderLab::Graph::MarkNodeChanged(*node);
        };
        auto pixel = [&](uint32_t nodeId)
        {
            auto r = ShaderLab::Rendering::ReadPixelRegion(g, nodeId, 8, 8, 1, 1, g_dc.get());
            auto* n = g.FindNode(nodeId);
            if (r.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success)
                printf("    (debug) node %u read status %d, output %p, error '%ls'\n", nodeId,
                       static_cast<int>(r.status), static_cast<void*>(n->cachedOutput), n->runtimeError.c_str());
            return (r.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success && r.pixels.size() >= 4)
                ? r.pixels[0] : -1.0f;
        };
        // Readbacks a frame performed: its dispatches less the skipped ones.
        struct Transport { uint32_t dispatches; uint64_t readbacks; };
        auto frame = [&]() -> Transport
        {
            const uint64_t skippedBefore = ShaderLab::Performance::SkippedCpuReadbacks();
            const auto result = ShaderLab::Rendering::FrameExecutor::Run(g, evaluator, g_dc.get(), {});
            const uint64_t skipped = ShaderLab::Performance::SkippedCpuReadbacks() - skippedBefore;
            return { result.dispatches, result.dispatches - skipped };
        };
        auto statsMax = [&]() { return AnalysisField(g.FindNode(statsId), L"Max"); };

        // ---- the first frame of a fresh graph, then three unchanged ones ----
        setLevel(2.0f);
        const Transport first = frame();
        const uint32_t firstBits = evaluator.AppliedGpuBits(gpuId);
        const float firstFieldsMax = statsMax();
        const float firstPx = pixel(gpuId);
        Transport replays{ 0, 0 };
        for (int i = 0; i < 3; ++i)
        {
            const Transport t = frame();
            replays.dispatches += t.dispatches;
            replays.readbacks += t.readbacks;
        }
        const float replayPx = pixel(gpuId);
        printf("  [info] first frame: %u dispatch(es), %llu readback(s), bits %u, pixel %.4f; "
               "3 replays: %u dispatch(es), %llu readback(s)\n",
               first.dispatches, first.readbacks, firstBits, firstPx, replays.dispatches, replays.readbacks);
        TEST("NoReadback_FirstFrameDispatchedOnce", first.dispatches == 1);
        TEST("NoReadback_FirstFrameReadsNothingBack", first.readbacks == 0 && firstFieldsMax < 0.0f);
        TEST("NoReadback_FirstFrameServedOnGpu", firstBits == 2u);
        TEST("NoReadback_ReplaysDoNotRedispatch", replays.dispatches == 0 && replays.readbacks == 0);
        TEST("NoReadback_ReplaysKeepThePixel", firstPx > 1.0f && replayPx == firstPx);

        // ---- a changed input: the new value, still GPU only ----
        setLevel(5.0f);
        const Transport changed = frame();
        const float changedPx = pixel(gpuId);
        printf("  [info] changed input: %u dispatch(es), %llu readback(s), pixel %.4f -> %.4f\n",
               changed.dispatches, changed.readbacks, firstPx, changedPx);
        TEST("NoReadback_ChangedInputDispatches", changed.dispatches == 1 && changed.readbacks == 0);
        TEST("NoReadback_ChangedInputMovesThePixel", changedPx > firstPx * 1.5f);

        // ---- a CPU consumer bound to the clean producer reads its GPU results ----
        const auto cpuId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(cpuConsumer));
        g.Connect(gradientId, 0, cpuId, 0);
        g.BindProperty(cpuId, L"Level", statsId, L"Max", 0);
        const Transport bound = frame();
        const float cpuPx = pixel(cpuId);
        const float boundMax = statsMax();
        printf("  [info] CPU consumer bound: %u dispatch(es); Max %.4f, its pixel %.4f, GPU consumer %.4f\n",
               bound.dispatches, boundMax, cpuPx, pixel(gpuId));
        TEST("NoReadback_CpuConsumerNeedsNoRedispatch", bound.dispatches == 0);
        TEST("NoReadback_CpuConsumerGetsCurrentValue",
             boundMax > 1.0f && std::abs(cpuPx - boundMax) <= 1e-3f * boundMax &&
             std::abs(cpuPx - changedPx) <= 1e-3f * changedPx);

        // ---- with a CPU consumer present, a change reads back for it ----
        setLevel(3.0f);
        const Transport shared = frame();
        const float sharedGpuPx = pixel(gpuId);
        const float sharedCpuPx = pixel(cpuId);
        printf("  [info] shared producer: %u readback(s); GPU consumer %.4f, CPU consumer %.4f, Max %.4f\n",
               static_cast<unsigned>(shared.readbacks), sharedGpuPx, sharedCpuPx, statsMax());
        TEST("NoReadback_SharedProducerReadsBackOnce", shared.dispatches == 1 && shared.readbacks == 1);
        TEST("NoReadback_SharedProducerKeepsGpuBinding", evaluator.AppliedGpuBits(gpuId) == 2u);
        TEST("NoReadback_SharedProducerBothCurrent",
             std::abs(sharedCpuPx - statsMax()) <= 1e-3f * statsMax() &&
             std::abs(sharedGpuPx - sharedCpuPx) <= 1e-3f * sharedCpuPx && sharedCpuPx < changedPx);
        evaluator.ReleaseCache(g);
    }

    // A D3D11 compute producer whose fields are a FloatArray (packed four per
    // pixel), a scalar after it, and a Float3. Checks the bridge readback
    // unpacks the packed layout, and that a binding to the Float3's .y gives
    // the same value on the GPU lane as on the CPU path.
    SL_TEST_CASE(TestAnalysisFieldLayoutAndComponentBinding, Tag::Gpu)
    {
        printf("\n=== Analysis field layout and component bindings ===\n");
        using namespace ShaderLab::Effects;
        auto& registry = ShaderLabEffects::Instance();

        ShaderLabEffectDescriptor producer;
        producer.name = L"Field Layout Test Producer";
        producer.effectId = L"Field Layout Test Producer"; producer.effectVersion = 1;
        producer.shaderType = ShaderLab::Graph::CustomShaderType::D3D11ComputeShader;
        producer.dataOnly = true;
        producer.threadGroupX = producer.threadGroupY = producer.threadGroupZ = 1;
        producer.inputNames = { L"Source" };
        producer.hlslSource = R"HLSL(
Texture2D<float4> Source : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
};
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const float unused = (float)(Width + Height) * 0.0;
    Result[0] = float4(10.0, 11.0, 12.0, 13.0) + unused;
    Result[1] = float4(14.0, 15.0, 0.0, 0.0);
    Result[2] = float4(99.0, 0.0, 0.0, 0.0);
    Result[3] = float4(5.0, 7.0, 11.0, 0.0);
}
)HLSL";
        producer.analysisOutputType = ShaderLab::Graph::AnalysisOutputType::Typed;
        producer.analysisFields = {
            { L"Packed", ShaderLab::Graph::AnalysisFieldType::FloatArray, 6 },
            { L"After",  ShaderLab::Graph::AnalysisFieldType::Float },
            { L"Triple", ShaderLab::Graph::AnalysisFieldType::Float3 },
        };

        ShaderLabEffectDescriptor consumer;
        consumer.name = L"Field Layout Test Consumer";
        consumer.effectId = L"Field Layout Test Consumer"; consumer.effectVersion = 1;
        consumer.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        consumer.inputNames = { L"Source" };
        consumer.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Level, t1)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    float a = Source.Sample(InputSampler, uv0.xy).a;
    return float4(Level, _SLPARAM_Level_GPU ? 1.0 : 0.0, 0.0, a);
}
)HLSL";
        consumer.parameters = {
            ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", true },
        };

        std::vector<ShaderLab::Graph::AnalysisFieldValue> producerFields;
        uint32_t consumerAppliedBits = 0;
        // Returns the bound value and whether the GPU-binding build ran.
        auto run = [&](bool gpu, uint32_t component) -> std::pair<float, bool>
        {
            CaseEvaluator().ReleaseCache();
            ShaderLab::Performance::SetGpuBindingsEnabled(gpu);
            ShaderLab::Graph::GraphModel graph;
            const auto sourceId = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
            const auto producerId = graph.AddNode(ShaderLabEffects::CreateNode(producer));
            const auto consumerId = graph.AddNode(ShaderLabEffects::CreateNode(consumer));
            graph.Connect(sourceId, 0, producerId, 0);
            graph.Connect(sourceId, 0, consumerId, 0);
            graph.BindProperty(consumerId, L"Level", producerId, L"Triple", component);
            for (int i = 0; i < 4; ++i)
            {
                graph.MarkAllDirty();
                RunFrame(graph);
            }
            producerFields = graph.FindNode(producerId)->analysisOutput.fields;
            consumerAppliedBits = CaseEvaluator().AppliedGpuBits(consumerId);
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, consumerId, 8, 8, 1, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 4)
                return { -1.0f, false };
            return { readback.pixels[0], readback.pixels[1] >= 0.5f };
        };

        // Readbacks skipped, as in the GUI.
        ScopedPerformanceSettings settings;
        ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
        ShaderLab::Performance::SetAsyncAnalysisReadbackEnabled(false);
        const auto [cpuValue, cpuUsedGpuBuild] = run(false, 1);
        const auto findField = [&](const wchar_t* name) -> const ShaderLab::Graph::AnalysisFieldValue* {
            for (const auto& field : producerFields)
                if (field.name == name) return &field;
            return nullptr;
        };
        const auto* packed = findField(L"Packed");
        const auto* after = findField(L"After");
        const auto* triple = findField(L"Triple");
        const std::vector<float> expectedPacked = { 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f };
        TEST("FieldLayout_FloatArrayUnpackedFourPerPixel", packed && packed->arrayData == expectedPacked);
        TEST("FieldLayout_ScalarAfterFloatArray", after && after->components[0] == 99.0f);
        TEST("FieldLayout_Float3AfterScalar", triple &&
             triple->components[0] == 5.0f && triple->components[1] == 7.0f && triple->components[2] == 11.0f);
        // Instrument: a .x binding must run the GPU-binding build.
        const auto [gpuValueX, gpuBuildX] = run(true, 0);
        const auto [gpuValueY, gpuBuildY] = run(true, 1);
        // Lane 3 does not depend on skipping readbacks: headless and the tests
        // read back every dispatch and must still run the texture build.
        ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
        const auto [noSkipValueX, noSkipBuildX] = run(true, 0);
        const uint32_t noSkipAppliedBits = consumerAppliedBits;
        CaseEvaluator().ReleaseCache();

        printf("  [info] Triple.x with GPU bindings on=%.3f (gpu build %d); Triple.y via cbuffer=%.3f, with GPU bindings on=%.3f (gpu build %d)\n",
               gpuValueX, gpuBuildX ? 1 : 0, cpuValue, gpuValueY, gpuBuildY ? 1 : 0);
        TEST("ComponentBinding_GpuLaneServesX", gpuValueX == 5.0f && gpuBuildX);
        TEST("ComponentBinding_CpuPathReadsY", cpuValue == 7.0f && !cpuUsedGpuBuild);
        TEST("ComponentBinding_GpuSettingAgreesWithCpu", gpuValueY == cpuValue);
        printf("  [info] without the readback skip: Triple.x=%.3f (gpu build %d), applied bits %u\n",
               noSkipValueX, noSkipBuildX ? 1 : 0, noSkipAppliedBits);
        TEST("Lane3_ServedWithoutReadbackSkip", noSkipValueX == 5.0f && noSkipBuildX);
        TEST("Lane3_AppliedTextureModeWithoutReadbackSkip", noSkipAppliedBits == 2u);
    }

    // While a node's GPU-binding variant compiles (async, as in the GUI) it
    // renders a cbuffer build, so its bound analysis values must still be read
    // back each frame even though the producer's texture exists. The shader
    // writes Level to red and which build ran to green; every non-baseline
    // build is slow to compile, so the fallback runs for many frames.
    SL_TEST_CASE(TestFallbackBuildBindingReadback, Tag::Gpu)
    {
        printf("\n=== Fallback build reads this frame's bound value ===\n");
        using namespace ShaderLab::Effects;
        auto& registry = ShaderLabEffects::Instance();
        ShaderLabEffectDescriptor descriptor;
        descriptor.name = L"Fallback Binding Test";
        descriptor.effectId = L"Fallback Binding Test"; descriptor.effectVersion = 1;
        descriptor.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        descriptor.inputNames = { L"Source" };
        descriptor.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Level, t1)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
    SHADERLAB_OPTION(uint, Mode)
};
SHADERLAB_OPTION_VALUE(uint, Mode)
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0, float4 uv1 : TEXCOORD1) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    const float mode = (float)Mode;
    const float alpha = Source.Sample(InputSampler, uv0.xy).a;
    float busy = 0.0;
#if _SLOPT_Mode_MODE || _SLPARAM_Level_GPU
    // Slow to compile, so the baseline is shown while the variants build.
    [unroll] for (int i = 0; i < 1000; ++i) busy = frac(sin(busy * 1.31 + i * 0.7 + scenePos.x) * 43758.5);
    [unroll] for (int j = 0; j < 1000; ++j) busy = frac(cos(busy * 1.17 + j * 0.3 + scenePos.y) * 23421.6);
#endif
    const float build = (_SLOPT_Mode_MODE ? 1.0 : 0.0) + (_SLPARAM_Level_GPU ? 2.0 : 0.0);
    return float4(Level, build, busy * 1e-9 + mode, alpha);
}
)HLSL";
        ShaderLab::Graph::ParameterDefinition level{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", true };
        ShaderLab::Graph::ParameterDefinition mode{ L"Mode", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"A", L"B" } };
        mode.specialize = true;
        descriptor.parameters = { level, mode };

        const float levels[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        auto setLevel = [&](ShaderLab::Graph::GraphModel& graph, uint32_t gradientId, float value) {
            auto* gradient = graph.FindNode(gradientId);
            for (const wchar_t* key : { L"StartR", L"StartG", L"StartB", L"EndR", L"EndG", L"EndB" })
                ShaderLab::Graph::SetNodeProperty(*gradient, key, value);
            ShaderLab::Graph::MarkNodeChanged(*gradient);
        };
        auto addGradient = [&](ShaderLab::Graph::GraphModel& graph) {
            auto gradient = ShaderLabEffects::CreateNode(*registry.FindByName(L"Gradient Generator"));
            ShaderLab::Graph::SetNodeProperty(gradient, L"GradSize", 64.0f);
            return graph.AddNode(std::move(gradient));
        };
        auto frame = [&](ShaderLab::Graph::GraphModel& graph) {
            RunFrame(graph, ShaderLab::Rendering::SecondPass::Never);
        };
        auto statsMax = [](const ShaderLab::Graph::EffectNode* stats) { return AnalysisField(stats, L"Max"); };

        // Instrument: the Max each level produces, read back on the CPU.
        ScopedPerformanceSettings settings;
        ShaderLab::Performance::SetGpuBindingsEnabled(false);
        ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);
        ShaderLab::Performance::SetAsyncAnalysisReadbackEnabled(false);
        CaseEvaluator().ReleaseCache();
        CaseEvaluator().ClearCpuAnalysisInterest();
        float expectedMax[4] = {};
        {
            ShaderLab::Graph::GraphModel graph;
            const auto gradientId = addGradient(graph);
            const auto statsId = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Luminance Statistics")));
            graph.Connect(gradientId, 0, statsId, 0);
            for (int i = 0; i < 4; ++i)
            {
                setLevel(graph, gradientId, levels[i]);
                frame(graph);
                frame(graph);
                expectedMax[i] = statsMax(graph.FindNode(statsId));
            }
        }
        const bool levelsDistinct = expectedMax[0] > 0.0f && expectedMax[0] < expectedMax[1] &&
                                    expectedMax[1] < expectedMax[2] && expectedMax[2] < expectedMax[3];

        // The GUI's mode: async compile, GPU bindings on, unneeded readbacks skipped.
        CaseEvaluator().ReleaseCache();
        ShaderLab::Performance::SetGpuBindingsEnabled(true);
        ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
        CaseEvaluator().SetAsyncCompile(true);
        ShaderLab::Graph::GraphModel graph;
        const auto gradientId = addGradient(graph);
        const auto statsId = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Luminance Statistics")));
        auto consumer = ShaderLabEffects::CreateNode(descriptor);
        consumer.customEffect->hlslSource += L"\n// fallback binding test " +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"\n";
        consumer.customEffect->compiledBytecode.clear();
        const auto consumerId = graph.AddNode(std::move(consumer));
        graph.Connect(gradientId, 0, statsId, 0);
        graph.Connect(gradientId, 0, consumerId, 0);
        graph.BindProperty(consumerId, L"Level", statsId, L"Max", 0);

        int fallbackFrames = 0, fallbackWrong = 0, variantFrames = 0, variantWrong = 0;
        float firstWrongValue = 0.0f, firstWrongExpected = 0.0f;
        const auto start = std::chrono::steady_clock::now();
        for (int frameIndex = 0; variantFrames < 8 && std::chrono::steady_clock::now() - start < std::chrono::seconds(60); ++frameIndex)
        {
            const int levelIndex = frameIndex % 4;
            setLevel(graph, gradientId, levels[levelIndex]);
            frame(graph);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!graph.FindNode(consumerId)->cachedOutput) continue;
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, consumerId, 8, 8, 1, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 4) continue;
            const float value = readback.pixels[0];
            const float expected = expectedMax[levelIndex];
            const bool gpuBuild = readback.pixels[1] >= 1.5f;   // build 2 or 3: Level from the texture
            const bool right = std::abs(value - expected) <= 1e-3f * expected;
            if (gpuBuild) { ++variantFrames; if (!right) ++variantWrong; continue; }
            ++fallbackFrames;
            if (!right && fallbackWrong++ == 0) { firstWrongValue = value; firstWrongExpected = expected; }
        }
        CaseEvaluator().SetAsyncCompile(false);
        CaseEvaluator().ReleaseCache();

        printf("  [info] Max per level %.2f %.2f %.2f %.2f; fallback frames %d (%d wrong, first %.2f for %.2f), variant frames %d (%d wrong)\n",
               expectedMax[0], expectedMax[1], expectedMax[2], expectedMax[3], fallbackFrames, fallbackWrong,
               firstWrongValue, firstWrongExpected, variantFrames, variantWrong);
        TEST("FallbackBinding_InstrumentLevelsDistinct", levelsDistinct);
        TEST("FallbackBinding_FallbackWindowExercised", fallbackFrames >= 4);
        TEST("FallbackBinding_FallbackReadsThisFramesValue", fallbackFrames > 0 && fallbackWrong == 0);
        TEST("FallbackBinding_VariantReadsThisFramesValue", variantFrames > 0 && variantWrong == 0);
    }

    // Skipping unneeded CPU readback with GPU bindings on: Source -> Luminance
    // Statistics -> ICtCp Tone Map (TargetPeakNits bound to Mean). With the skip
    // on, the statistics dispatch's CPU Map is elided and the tone map still
    // renders from the GPU texture; a host interest hint brings the readback
    // back, and with the skip off it always runs.
    SL_TEST_CASE(TestSkipCpuReadback, Tag::Gpu)
    {
        printf("\n=== Skip CPU readback ===\n");
        CaseEvaluator().ClearCpuAnalysisInterest();

        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        auto* srcDesc   = registry.FindByName(L"Gamut Source");
        auto* statsDesc = registry.FindByName(L"Luminance Statistics");
        auto* ictcpDesc = registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)");
        if (!srcDesc || !statsDesc || !ictcpDesc) {
            TEST("SkipReadback_DescriptorsFound", false);
            return;
        }

        auto buildGraph = [&](ShaderLab::Graph::GraphModel& g,
                              uint32_t& srcId, uint32_t& statsId, uint32_t& ictcpId) {
            auto srcNode   = ShaderLab::Effects::ShaderLabEffects::CreateNode(*srcDesc);
            auto statsNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*statsDesc);
            auto ictcpNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(*ictcpDesc);
            srcId   = g.AddNode(std::move(srcNode));
            statsId = g.AddNode(std::move(statsNode));
            ictcpId = g.AddNode(std::move(ictcpNode));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, ictcpId, 0);
            g.BindProperty(ictcpId, L"TargetPeakNits", statsId, L"Mean", 0);
        };

        // ---- Sub-test 1: skip flag ON + GPU flag ON, no host hint -------
        // LumStats.Mean is read by ICtCp via GPU SRV -> readback skipped.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::GraphModel g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            uint64_t beforeSkip = ShaderLab::Performance::SkippedCpuReadbacks();

            // Two-pass: first creates effects, second produces output.
            PrepareSourcesAndEvaluate(g, sf);
            g_dc->SetTarget(nullptr);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

            uint64_t afterSkip = ShaderLab::Performance::SkippedCpuReadbacks();
            TEST("SkipReadback_CounterIncrementsWhenSkipping",
                 afterSkip > beforeSkip);

            // ICtCp's image output must still be valid (it reads
            // LumStats.Mean via the upstream SRV at t1).
            auto* ictcp = g.FindNode(ictcpId);
            TEST("SkipReadback_ICtCpHasOutput",
                 ictcp && ictcp->cachedOutput != nullptr);
        }

        // ---- Sub-test 2: skip flag ON + host hint forces readback ------
        // Adding LumStats to CpuAnalysisInterest re-engages readback so
        // the Properties panel / MCP can read fresh values. Validates
        // the host's escape hatch for any node that needs CPU values.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::GraphModel g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            CaseEvaluator().SetCpuAnalysisInterest({ statsId });

            PrepareSourcesAndEvaluate(g, sf);
            g_dc->SetTarget(nullptr);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

            const float mean = AnalysisField(g.FindNode(statsId), L"Mean");
            TEST("SkipReadback_HostHintForcesReadback", mean > 0.0f);

            CaseEvaluator().ClearCpuAnalysisInterest();
        }

        // ---- Sub-test 3: skip flag OFF preserves baseline ---------------
        // Default behavior: every dispatch reads back regardless.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::GraphModel g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(false);

            PrepareSourcesAndEvaluate(g, sf);
            g_dc->SetTarget(nullptr);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

            const float mean = AnalysisField(g.FindNode(statsId), L"Mean");
            TEST("SkipReadback_FlagOffAlwaysReadsBack", mean > 0.0f);
        }

        // ---- Sub-test 4: hint throttle ---------------------------------
        // With throttle large, repeated frames with the same hint do NOT
        // re-readback so SkippedCpuReadbacks() advances. With throttle 0,
        // every frame reads back so the skipped counter stays put.
        {
            ShaderLab::Effects::SourceNodeFactory sf;
            ShaderLab::Graph::GraphModel g;
            uint32_t srcId, statsId, ictcpId;
            buildGraph(g, srcId, statsId, ictcpId);
            // Drop the LumStats <- ICtCp binding so LumStats is purely
            // a host-hint readback target (no CPU-routed binding consumer
            // would force readback every frame).
            g.UnbindProperty(ictcpId, L"TargetPeakNits");

            ShaderLab::Performance::SetGpuBindingsEnabled(true);
            ShaderLab::Performance::SetSkipUnneededCpuReadbackEnabled(true);
            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(60'000);
            CaseEvaluator().SetCpuAnalysisInterest({ statsId });

            // First eval pair (fresh hint -> read).
            PrepareSourcesAndEvaluate(g, sf);
            g_dc->SetTarget(nullptr);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());

            uint64_t beforeSkipped = ShaderLab::Performance::SkippedCpuReadbacks();
            for (int i = 0; i < 4; ++i)
            {
                PrepareSourcesAndEvaluate(g, sf);
                CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            }
            uint64_t afterSkipped = ShaderLab::Performance::SkippedCpuReadbacks();
            TEST("SkipReadback_HintThrottleSkipsRepeatFrames",
                 afterSkipped > beforeSkipped);

            ShaderLab::Performance::SetCpuAnalysisHintThrottleMs(0);
            // With throttle=0 every frame should re-read LumStats, so the
            // hinted node gets a fresh value. Mutate fields then check
            // it gets repopulated by the next ProcessDeferredCompute.
            auto* statsNode = g.FindNode(statsId);
            if (statsNode)
                statsNode->analysisOutput.fields.clear();
            PrepareSourcesAndEvaluate(g, sf);
            CaseEvaluator().ProcessDeferredCompute(g, g_dc.get());
            bool refreshed = statsNode &&
                statsNode->analysisOutput.type == ShaderLab::Graph::AnalysisOutputType::Typed &&
                !statsNode->analysisOutput.fields.empty();
            TEST("SkipReadback_ThrottleZeroReadsHintEveryFrame", refreshed);

            CaseEvaluator().ClearCpuAnalysisInterest();
        }
    }

    // Each evaluator renders with its own options: pinned ones survive a
    // change to the process defaults, and two evaluators in one process can
    // route the same binding differently in alternating frames.
    SL_TEST_CASE(TestEvaluatorOptionsPerInstance, Tag::Gpu)
    {
        printf("\n=== Evaluator options are per instance ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        {
            ShaderLab::Rendering::GraphEvaluator evaluator;
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
            TEST("EvalOptions_FollowsDefaults", !evaluator.Options().gpuBindings);
            auto pinned = evaluator.Options();
            pinned.gpuBindings = true;
            evaluator.SetOptions(pinned);
            ShaderLab::Performance::SetGpuBindingsEnabled(false);
            TEST("EvalOptions_PinnedIgnoresDefaults", evaluator.Options().gpuBindings);
            evaluator.FollowDefaultOptions();
            TEST("EvalOptions_FollowsDefaultsAgain", !evaluator.Options().gpuBindings);
        }

        auto build = [&](ShaderLab::Graph::GraphModel& g, uint32_t& tmId)
        {
            auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Gamut Source")));
            auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"Luminance Statistics")));
            tmId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
                *registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)")));
            g.Connect(srcId, 0, statsId, 0);
            g.Connect(srcId, 0, tmId, 0);
            g.BindProperty(tmId, L"SourcePeakNits", statsId, L"Max", 0);
        };

        // The process default says CPU; the first evaluator is pinned to GPU.
        ShaderLab::Performance::SetGpuBindingsEnabled(false);
        ShaderLab::Rendering::GraphEvaluator gpuEvaluator, cpuEvaluator;
        auto gpuOptions = ShaderLab::Performance::DefaultEvaluatorOptions();
        gpuOptions.gpuBindings = true;
        gpuEvaluator.SetOptions(gpuOptions);

        ShaderLab::Graph::GraphModel gpuGraph, cpuGraph;
        uint32_t gpuConsumer = 0, cpuConsumer = 0;
        build(gpuGraph, gpuConsumer);
        build(cpuGraph, cpuConsumer);
        for (int i = 0; i < 4; ++i)
        {
            ShaderLab::Rendering::FrameRequest request;
            request.markAllDirty = true;
            ShaderLab::Rendering::FrameExecutor::Run(gpuGraph, gpuEvaluator, g_dc.get(), request);
            ShaderLab::Rendering::FrameExecutor::Run(cpuGraph, cpuEvaluator, g_dc.get(), request);
        }
        const uint32_t gpuBits = gpuEvaluator.AppliedGpuBits(gpuConsumer);
        const uint32_t cpuBits = cpuEvaluator.AppliedGpuBits(cpuConsumer);
        printf("  [info] applied GPU-binding bits: pinned evaluator %u, default evaluator %u\n", gpuBits, cpuBits);
        TEST("EvalOptions_PinnedEvaluatorServesOnGpu", gpuBits != 0);
        TEST("EvalOptions_DefaultEvaluatorStaysOnCpu", cpuBits == 0);
        TEST("EvalOptions_BothRendered",
             gpuGraph.FindNode(gpuConsumer)->cachedOutput && cpuGraph.FindNode(cpuConsumer)->cachedOutput);
        gpuEvaluator.ReleaseCache(gpuGraph);
        cpuEvaluator.ReleaseCache(cpuGraph);
    }

    // RequestFreshCpuReadback reads a node back, blocking, on its next
    // dispatch even while readbacks are skipped and queued asynchronously,
    // and only on that one: the /analysis route relies on both halves.
    SL_TEST_CASE(TestFreshCpuReadbackRequest, Tag::Gpu)
    {
        printf("\n=== Fresh CPU readback on request ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Rendering::GraphEvaluator evaluator;
        auto options = ShaderLab::Performance::DefaultEvaluatorOptions();
        options.skipUnneededCpuReadback = true;
        options.asyncAnalysisReadback = true;
        evaluator.SetOptions(options);

        ShaderLab::Graph::GraphModel g;
        auto gradient = ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gradient Generator"));
        ShaderLab::Graph::SetNodeProperty(gradient, L"GradSize", 64.0f);
        const auto gradientId = g.AddNode(std::move(gradient));
        const auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        g.Connect(gradientId, 0, statsId, 0);
        auto setLevel = [&](float value)
        {
            auto* node = g.FindNode(gradientId);
            for (const wchar_t* key : { L"StartR", L"StartG", L"StartB", L"EndR", L"EndG", L"EndB" })
                ShaderLab::Graph::SetNodeProperty(*node, key, value);
            ShaderLab::Graph::MarkNodeChanged(*node);
        };
        auto statsMax = [&]() { return AnalysisField(g.FindNode(statsId), L"Max"); };
        auto frame = [&]()
        {
            ShaderLab::Rendering::FrameRequest request;
            ShaderLab::Rendering::FrameExecutor::Run(g, evaluator, g_dc.get(), request);
        };

        setLevel(1.0f);
        for (int i = 0; i < 3; ++i) { g.MarkAllDirty(); frame(); }
        TEST("FreshReadback_SkippedWithoutRequest", statsMax() < 0.0f);

        ShaderLab::Graph::MarkNodeChanged(*g.FindNode(statsId));
        evaluator.RequestFreshCpuReadback(statsId);
        frame();
        const float first = statsMax();
        TEST("FreshReadback_RequestReadsBack", first > 0.0f);
        TEST("FreshReadback_RequestBlocks", !evaluator.HasPendingReadbacks());

        setLevel(4.0f);
        for (int i = 0; i < 3; ++i) frame();
        TEST("FreshReadback_RequestIsOneShot", statsMax() == first);

        ShaderLab::Graph::MarkNodeChanged(*g.FindNode(statsId));
        evaluator.RequestFreshCpuReadback(statsId);
        frame();
        printf("  [info] Max at level 1: %.3f, after level 4 on request: %.3f\n", first, statsMax());
        TEST("FreshReadback_SecondRequestSeesNewValue", statsMax() > first);
        evaluator.ReleaseCache(g);
    }

    // Per-node evaluator state goes as one record: invalidating a lane-3
    // producer drops the texture view its consumers bind, a node's output
    // change count survives invalidation, and a node removed from the graph
    // leaves nothing behind.
    SL_TEST_CASE(TestEvaluatorNodeRuntime, Tag::Gpu)
    {
        printf("\n=== Evaluator per-node state ===\n");
        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();

        ShaderLab::Effects::ShaderLabEffectDescriptor consumer;
        consumer.name = L"Node Runtime Test Consumer";
        consumer.effectId = L"Node Runtime Test Consumer"; consumer.effectVersion = 1;
        consumer.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        consumer.inputNames = { L"Source" };
        consumer.hlslSource = R"HLSL(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Level, t1)
SamplerState InputSampler : register(s0);
cbuffer Constants : register(b0)
{
    SHADERLAB_PARAM(float, Level)
};
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION,
            float4 uv0 : TEXCOORD0) : SV_TARGET
{
    SHADERLAB_LOAD_PARAM(float, Level)
    return float4(Level, Level, Level, Source.Sample(InputSampler, uv0.xy).a);
}
)HLSL";
        consumer.parameters = {
            ShaderLab::Graph::ParameterDefinition{ L"Level", L"float", 0.0f, 0.0f, 10000.0f, 1.0f, {}, L"", true },
        };

        ShaderLab::Rendering::GraphEvaluator evaluator;
        auto options = ShaderLab::Performance::DefaultEvaluatorOptions();
        options.gpuBindings = true;
        evaluator.SetOptions(options);

        ShaderLab::Graph::GraphModel g;
        const auto srcId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
        const auto statsId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        const auto consId = g.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(consumer));
        g.Connect(srcId, 0, statsId, 0);
        g.Connect(srcId, 0, consId, 0);
        g.BindProperty(consId, L"Level", statsId, L"Max", 0);
        auto frame = [&]()
        {
            ShaderLab::Rendering::FrameRequest request;
            request.markAllDirty = true;
            ShaderLab::Rendering::FrameExecutor::Run(g, evaluator, g_dc.get(), request);
        };
        for (int i = 0; i < 4; ++i) frame();
        const uint32_t servedBits = evaluator.AppliedGpuBits(consId);
        const uint64_t changesBefore = evaluator.OutputChangeCount(statsId);

        // The producer's next bridge has published nothing yet, so a consumer
        // evaluated in between reads its CPU value, not the old bridge's texture.
        evaluator.InvalidateNode(g, statsId);
        evaluator.Evaluate(g, g_dc.get());
        const uint32_t bitsAfterInvalidate = evaluator.AppliedGpuBits(consId);
        TEST("NodeRuntime_ChangeCountSurvivesInvalidate", evaluator.OutputChangeCount(statsId) > changesBefore);
        for (int i = 0; i < 4; ++i) frame();
        const uint32_t bitsAfterRepublish = evaluator.AppliedGpuBits(consId);
        printf("  [info] consumer bits: served %u, after invalidating the producer %u, after it republished %u\n",
               servedBits, bitsAfterInvalidate, bitsAfterRepublish);
        TEST("NodeRuntime_TextureServedBeforeInvalidate", servedBits == 2u);
        TEST("NodeRuntime_InvalidateDropsLane3View", bitsAfterInvalidate == 0u);
        TEST("NodeRuntime_TextureServedAfterRepublish", bitsAfterRepublish == 2u);

        g.RemoveNode(consId);
        frame();
        TEST("NodeRuntime_RemovedNodeForgotten",
             evaluator.AppliedGpuBits(consId) == 0u && evaluator.OutputChangeCount(consId) == 0u);
        evaluator.ReleaseCache(g);

        // Bytecode the device refuses as a compute shader (here a pixel
        // shader) fails at that step, by name, on every frame.
        ShaderLab::Effects::ShaderLabEffectDescriptor compute;
        compute.name = L"Bridge Failure Test";
        compute.effectId = L"Bridge Failure Test"; compute.effectVersion = 1;
        compute.shaderType = ShaderLab::Graph::CustomShaderType::D3D11ComputeShader;
        compute.dataOnly = true;
        compute.inputNames = { L"Source" };
        compute.hlslSource = "// replaced by pixel-shader bytecode below";
        compute.analysisOutputType = ShaderLab::Graph::AnalysisOutputType::Typed;
        compute.analysisFields = { { L"Value", ShaderLab::Graph::AnalysisFieldType::Float } };
        auto pixel = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            "float4 main(float4 p : SV_POSITION) : SV_TARGET { return p; }", "BridgeFailurePs", "main", "ps_5_0");
        ShaderLab::Graph::GraphModel failGraph;
        const auto failSrc = failGraph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
        auto computeNode = ShaderLab::Effects::ShaderLabEffects::CreateNode(compute);
        if (pixel.succeeded && pixel.bytecode)
        {
            const auto* bytes = static_cast<const uint8_t*>(pixel.bytecode->GetBufferPointer());
            computeNode.customEffect->compiledBytecode.assign(bytes, bytes + pixel.bytecode->GetBufferSize());
        }
        const auto failId = failGraph.AddNode(std::move(computeNode));
        failGraph.Connect(failSrc, 0, failId, 0);
        std::wstring firstError, secondError;
        for (int i = 0; i < 2; ++i)
        {
            ShaderLab::Rendering::FrameRequest request;
            request.markAllDirty = true;
            ShaderLab::Rendering::FrameExecutor::Run(failGraph, evaluator, g_dc.get(), request);
            (i == 0 ? firstError : secondError) = failGraph.FindNode(failId)->runtimeError;
        }
        printf("  [info] refused shader: '%ls'\n", secondError.c_str());
        TEST("BridgeFailure_NamesShaderCreation",
             pixel.succeeded && firstError.find(L"creating the compute shader") != std::wstring::npos);
        TEST("BridgeFailure_RepeatsWithoutRetry", secondError == firstError);
        evaluator.ReleaseCache(failGraph);
    }
}
