#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/BytecodeCache.h"
#include "Effects/ShaderCompiler.h"
#include "Effects/ShaderVariants.h"
#include "Graph/GraphModel.h"
#include "Rendering/GraphEvaluator.h"
#include "Rendering/PixelReadback.h"
#include "Graph/GraphEdits.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// Shader variants: async compile, option specialisation and variant
// state through reloads.
namespace
{
    using namespace ShaderLab::Tests;

    // A node dirty on every frame never goes through D2D's PrepareForRender,
    // which only follows a property change, so a variant switch must load the
    // new shader itself. Each frame changes the option and the source and
    // evaluates once, as a playing video does.
    SL_TEST_CASE(TestVariantSwitchWhileDirty, Tag::Gpu)
    {
        printf("\n=== Variant switch on a node dirty every frame ===\n");
        using namespace ShaderLab::Effects;
        auto& registry = ShaderLabEffects::Instance();
        auto node = ShaderLabEffects::CreateNode(*registry.FindByName(L"ICtCp Round-Trip Validator"));
        auto& def = node.customEffect.value();
        def.hlslSource = LR"HLSL(
#include "shaderlab_params.hlsli"
cbuffer Constants : register(b0)
{
    SHADERLAB_OPTION(uint, Mode)   // 0 Red, 1 Green
    float Gain;
};
SHADERLAB_OPTION_VALUE(uint, Mode)
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 c = Source.Sample(InputSampler, uv0.xy);
    float3 col = (Mode == 0) ? float3(1, 0, 0) : float3(0, 1, 0);
    return float4(col * Gain + c.rgb * 0.0, _SLOPT_Mode_MODE ? 0.5 : 1.0);
}
)HLSL";
        def.compiledBytecode.clear();
        def.shaderLabEffectId = L"Test Variant Switch While Dirty";
        def.parameters.clear();
        ShaderLab::Graph::ParameterDefinition mode{ L"Mode", L"float", 0.0f, 0.0f, 1.0f, 1.0f, { L"Red", L"Green" } };
        mode.specialize = true;
        def.parameters.push_back(mode);
        def.parameters.push_back({ L"Gain", L"float", 1.0f, 0.0f, 4.0f, 0.01f });
        node.name = L"Test Variant Switch While Dirty";
        node.properties.clear();
        ShaderLab::Graph::SetNodeProperty(node, L"Mode", 0.0f);
        ShaderLab::Graph::SetNodeProperty(node, L"Gain", 1.0f);

        CaseEvaluator().ReleaseCache();
        ShaderLab::Graph::GraphModel graph;
        auto gradient = ShaderLabEffects::CreateNode(*registry.FindByName(L"Gradient Generator"));
        ShaderLab::Graph::SetNodeProperty(gradient, L"GradSize", 64.0f);
        const auto gradientId = graph.AddNode(std::move(gradient));
        const auto nodeId = graph.AddNode(std::move(node));
        graph.Connect(gradientId, 0, nodeId, 0);

        int frames = 0, wrong = 0;
        for (int frameIndex = 0; frameIndex < 12; ++frameIndex)
        {
            const float modeValue = static_cast<float>(frameIndex % 2);
            ShaderLab::Graph::SetNodeProperty(*graph.FindNode(nodeId), L"Mode", modeValue);
            ShaderLab::Graph::MarkNodeChanged(*graph.FindNode(nodeId));
            ShaderLab::Graph::SetNodeProperty(*graph.FindNode(gradientId), L"StartR", 0.1f * static_cast<float>(frameIndex));
            ShaderLab::Graph::MarkNodeChanged(*graph.FindNode(gradientId));
            g_dc->SetTarget(nullptr);
            CaseEvaluator().Evaluate(graph, g_dc.get());
            if (frameIndex < 2) continue;   // new effects need two passes
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, 8, 8, 1, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 4) { ++wrong; continue; }
            ++frames;
            const float expectRed = modeValue == 0.0f ? 1.0f : 0.0f;
            if (std::abs(readback.pixels[0] - expectRed) > 2e-3f || std::abs(readback.pixels[1] - (1.0f - expectRed)) > 2e-3f ||
                std::abs(readback.pixels[3] - 0.5f) > 2e-3f)
            {
                if (wrong++ == 0)
                    printf("  [info] frame %d Mode %.0f read %.2f %.2f %.2f a%.2f\n", frameIndex, modeValue,
                           readback.pixels[0], readback.pixels[1], readback.pixels[2], readback.pixels[3]);
            }
        }
        CaseEvaluator().ReleaseCache();
        printf("  [info] %d frames read, %d showing the wrong variant\n", frames, wrong);
        TEST("VariantSwitch_DirtyEveryFrameLoadsNewShader", frames == 10 && wrong == 0);
    }

    // With async compile on (the GUI's mode) the evaluator never compiles on
    // its own thread: the node stays flagged pending until the worker delivers.
    SL_TEST_CASE(TestAsyncCompile, Tag::Gpu)
    {
        printf("\n=== Async shader compile ===\n");
        using ShaderLab::Effects::ShaderLabEffects;
        auto& registry = ShaderLabEffects::Instance();
        ShaderLab::Graph::GraphModel graph;
        auto source = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
        auto node = ShaderLabEffects::CreateNode(*registry.FindByName(L"ICtCp Round-Trip Validator"));
        // A unique comment so no cache already holds this source.
        node.customEffect->hlslSource += L"\n// async-compile test " +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"\n";
        node.customEffect->compiledBytecode.clear();
        auto nodeId = graph.AddNode(std::move(node));
        graph.Connect(source, 0, nodeId, 0);

        CaseEvaluator().SetAsyncCompile(true);
        auto evalOnce = [&] {
            g_dc->SetTarget(nullptr);
            CaseEvaluator().Evaluate(graph, g_dc.get());
        };
        evalOnce();
        const auto* evaluated = graph.FindNode(nodeId);
        const bool pendingFirst = evaluated->compilePending && evaluated->cachedOutput == nullptr && evaluated->dirty;
        const auto start = std::chrono::steady_clock::now();
        int frames = 0;
        while (graph.FindNode(nodeId)->compilePending && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            evalOnce();
            ++frames;
        }
        evalOnce();   // new effects need a second pass before their output is right
        evaluated = graph.FindNode(nodeId);
        printf("  [info] compiled in the background over %d frames, %.0f ms\n", frames,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        CaseEvaluator().SetAsyncCompile(false);
        TEST("AsyncCompile_FirstFrameIsPendingWithNoOutput", pendingFirst);
        TEST("AsyncCompile_FrameReturnsWhileCompiling", frames > 0);
        TEST("AsyncCompile_NodeRendersOnceReady", !evaluated->compilePending && evaluated->customEffect->isCompiled() &&
                                                  evaluated->cachedOutput != nullptr && evaluated->runtimeError.empty());
        CaseEvaluator().ReleaseCache();
    }

    // An option parameter marked `specialize` compiles one variant per value,
    // and the evaluator switches variants as the value changes. The shader
    // writes alpha 0.5 from a specialised build and 1.0 from the generic one,
    // so each check can see which build rendered.
    SL_TEST_CASE(TestOptionSpecialization, Tag::Gpu)
    {
        printf("\n=== Option specialisation: one shader variant per option value ===\n");
        using namespace ShaderLab::Effects;
        using ShaderLab::Graph::PropertyValue;
        using float3v = winrt::Windows::Foundation::Numerics::float3;
        auto& registry = ShaderLabEffects::Instance();
        static const wchar_t* scHlsl = LR"HLSL(
#include "shaderlab_params.hlsli"
cbuffer Constants : register(b0)
{
    SHADERLAB_OPTION(uint, Mode)   // 0 Red, 1 Green, 2 Custom
    float3 CustomColor;            // read only by Custom
    float  Gain;
};
SHADERLAB_OPTION_VALUE(uint, Mode)
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 c = Source.Sample(InputSampler, uv0.xy);
    float3 col = (Mode == 0) ? float3(1, 0, 0) : (Mode == 1) ? float3(0, 1, 0) : CustomColor;
    return float4(col * Gain + c.rgb * 0.0, _SLOPT_Mode_MODE ? 0.5 : 1.0);
}
)HLSL";
        auto makeDef = [&](ShaderLab::Graph::CustomEffectDefinition& def) {
            def.hlslSource = scHlsl;
            def.compiledBytecode.clear();
            def.shaderLabEffectId = L"Test Option Specialisation";
            def.parameters.clear();
            ShaderLab::Graph::ParameterDefinition mode{ L"Mode", L"float", 0.0f, 0.0f, 2.0f, 1.0f,
                                                        { L"Red", L"Green", L"Custom" } };
            mode.specialize = true;
            def.parameters.push_back(mode);
            def.parameters.push_back({ L"CustomColor", L"float3", float3v{ 0.2f, 0.4f, 0.6f }, 0.0f, 1.0f, 0.01f });
            def.parameters.push_back({ L"Gain", L"float", 1.0f, 0.0f, 4.0f, 0.01f });
        };

        // Option helpers
        ShaderLab::Graph::CustomEffectDefinition def;
        def.shaderType = ShaderLab::Graph::CustomShaderType::PixelShader;
        makeDef(def);
        TEST("OptSpec_NamesAndCombinations",
             SpecializedOptionNames(def) == std::vector<std::string>{ "Mode" } &&
             OptionCombinationCount(def) == 3 && AllOptionKeys(def).size() == 3);
        std::map<std::wstring, PropertyValue> values{ { L"Mode", 1.0f } };
        const bool keyExact = OptionKeyFor(def, values) == 2;        // slot 0 = option 1 + 1
        values[L"Mode"] = 1.5f;
        const bool keyFractional = OptionKeyFor(def, values) == 0;   // not an option: generic
        values[L"Mode"] = 7.0f;
        const bool keyOutOfRange = OptionKeyFor(def, values) == 0;
        TEST("OptSpec_KeyFromValues", keyExact && keyFractional && keyOutOfRange);
        // The generic build truncates: just below 1 reads as 0 there, so it must not pick variant 1.
        values[L"Mode"] = 0.99995f;
        const bool keyJustBelow = OptionKeyFor(def, values) == 0;
        values[L"Mode"] = 1.00005f;
        const bool keyJustAbove = OptionKeyFor(def, values) == 2;
        TEST("OptSpec_KeyAgreesWithGenericTruncation", keyJustBelow && keyJustAbove);
        {
            auto gpuRouted = def; gpuRouted.parameters[0].gpuBindable = true;     // GPU-routed: cannot specialise
            auto noLabels = def; noLabels.parameters[0].enumLabels.clear();       // no options: nothing to specialise
            TEST("OptSpec_OnlyCpuOptionParamsQualify",
                 SpecializedOptionNames(gpuRouted).empty() && SpecializedOptionNames(noLabels).empty());
        }
        TEST("OptSpec_SignatureUnchangedWithoutOptions",
             HashParamSignature({ "A" }, {}) == HashParamSignature({ "A" }) &&
             HashParamSignature({ "A" }, { "Mode" }) != HashParamSignature({ "A" }));

        // Every variant compiles and keeps the cbuffer layout
        {
            std::string hlsl(def.hlslSource.begin(), def.hlslSource.end());
            auto compileWith = [&](const char* mode, const char* value) {
                std::vector<ShaderCompiler::MacroDef> macros{ { "_SLOPT_Mode_MODE", mode }, { "_SLOPT_Mode", value } };
                return ShaderCompiler::CompileFromString(hlsl, "OptSpecTest", "main", "ps_5_0", macros);
            };
            // Byte offset of a cbuffer member, or -1 when it is absent.
            auto offsetOf = [](const ShaderCompileResult& result, const std::wstring& name) -> int {
                if (!result.bytecode) return -1;
                auto reflection = ShaderCompiler::Reflect(result.bytecode.get());
                if (reflection.constantBuffers.empty()) return -1;
                for (const auto& variable : reflection.constantBuffers[0].variables)
                    if (variable.name == name) return static_cast<int>(variable.offset);
                return -1;
            };
            auto generic = compileWith("0", "0");
            auto red = compileWith("1", "0"), custom = compileWith("1", "2");
            TEST("OptSpec_AllBuildsCompile", generic.succeeded && red.succeeded && custom.succeeded);
            TEST("OptSpec_LaterMembersKeepTheirOffset",
                 offsetOf(generic, L"CustomColor") >= 0 &&
                 offsetOf(generic, L"CustomColor") == offsetOf(red, L"CustomColor") &&
                 offsetOf(generic, L"Gain") == offsetOf(custom, L"Gain"));
            TEST("OptSpec_FixedVariantHasNoRuntimeSlot",
                 offsetOf(generic, L"Mode") == 0 && offsetOf(red, L"Mode") < 0);
        }

        // Rendering: variants switch, and non-option parameters stay live
        CaseEvaluator().ReleaseCache();
        ShaderLab::Graph::GraphModel graph;
        auto source = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
        auto node = ShaderLabEffects::CreateNode(*registry.FindByName(L"ICtCp Round-Trip Validator"));
        makeDef(node.customEffect.value());
        node.name = L"Test Option Specialisation";
        node.properties.clear();
        ShaderLab::Graph::SetNodeProperty(node, L"Mode", 0.0f);
        ShaderLab::Graph::SetNodeProperty(node, L"CustomColor", float3v{ 0.2f, 0.4f, 0.6f });
        ShaderLab::Graph::SetNodeProperty(node, L"Gain", 1.0f);
        const auto nodeId = graph.AddNode(std::move(node));
        graph.Connect(source, 0, nodeId, 0);
        auto render = [&](float mode, float3v custom, float gain) -> std::array<float, 4> {
            auto* specNode = graph.FindNode(nodeId);
            ShaderLab::Graph::SetNodeProperty(*specNode, L"Mode", mode);
            ShaderLab::Graph::SetNodeProperty(*specNode, L"CustomColor", custom);
            ShaderLab::Graph::SetNodeProperty(*specNode, L"Gain", gain);
            ShaderLab::Graph::MarkNodeChanged(*specNode);
            for (int i = 0; i < 2; ++i) { g_dc->SetTarget(nullptr); CaseEvaluator().Evaluate(graph, g_dc.get()); }
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, 8, 8, 1, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 4)
                return { -1, -1, -1, -1 };
            return { readback.pixels[0], readback.pixels[1], readback.pixels[2], readback.pixels[3] };
        };
        auto near4 = [](const std::array<float, 4>& actual, float expectR, float expectG, float expectB, float expectA) {
            return std::abs(actual[0] - expectR) < 2e-3f && std::abs(actual[1] - expectG) < 2e-3f &&
                   std::abs(actual[2] - expectB) < 2e-3f && std::abs(actual[3] - expectA) < 2e-3f;
        };
        const float3v defaultCustom{ 0.2f, 0.4f, 0.6f };
        auto red          = render(0.0f, defaultCustom, 1.0f);
        auto green        = render(1.0f, defaultCustom, 1.0f);
        auto customColor  = render(2.0f, defaultCustom, 2.0f);
        auto customEdited = render(2.0f, float3v{ 0.5f, 0.25f, 0.125f }, 2.0f);   // CustomColor edited, same variant
        auto back         = render(0.0f, defaultCustom, 0.5f);                     // back to an already-loaded variant
        auto fractional   = render(1.4f, defaultCustom, 1.0f);                     // not an option: generic build
        printf("  [info] red %.2f %.2f %.2f a%.2f | green a%.2f | custom %.2f %.2f %.2f | edited %.2f %.2f %.2f | back %.2f a%.2f | 1.4 -> %.2f %.2f a%.2f\n",
               red[0], red[1], red[2], red[3], green[3], customColor[0], customColor[1], customColor[2],
               customEdited[0], customEdited[1], customEdited[2], back[0], back[3], fractional[0], fractional[1], fractional[3]);
        TEST("OptSpec_EachOptionRendersItsVariant",
             near4(red, 1, 0, 0, 0.5f) && near4(green, 0, 1, 0, 0.5f) && near4(customColor, 0.4f, 0.8f, 1.2f, 0.5f));
        TEST("OptSpec_CustomCompanionStaysLive", near4(customEdited, 1.0f, 0.5f, 0.25f, 0.5f));
        TEST("OptSpec_SwitchesBackToAnEarlierVariant", near4(back, 0.5f, 0, 0, 0.5f));
        TEST("OptSpec_NonOptionValueUsesGenericBuild", near4(fractional, 0, 1, 0, 1.0f));

        // Async: every combination is precompiled, so a switch never waits
        {
            auto* specNode = graph.FindNode(nodeId);
            specNode->customEffect->hlslSource += L"\n// async variant test " +
                std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"\n";
            specNode->customEffect->compiledBytecode.clear();
            ShaderLab::Graph::SetNodeProperty(*specNode, L"Mode", 0.0f);
            ShaderLab::Graph::MarkNodeChanged(*specNode);
            CaseEvaluator().ReleaseCache();
            CaseEvaluator().SetAsyncCompile(true);
            // Count compiles with the cache's own counter: this shader compiles
            // too fast for variantsCompiling to be seen reliably. Showing only
            // Mode 0, the node must still build the baseline plus 3 variants.
            auto& cache = BytecodeCache::Instance();
            const auto compilesBefore = cache.GetStats().workerCompiles;
            const auto start = std::chrono::steady_clock::now();
            uint32_t peakCompiling = 0;
            auto evalOnce = [&] { g_dc->SetTarget(nullptr); CaseEvaluator().Evaluate(graph, g_dc.get()); };
            evalOnce();
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
            {
                specNode = graph.FindNode(nodeId);
                peakCompiling = (std::max)(peakCompiling, specNode->variantsCompiling);
                if (!specNode->compilePending && specNode->customEffect->isCompiled() && specNode->variantsCompiling == 0 &&
                    cache.GetStats().workerCompiles - compilesBefore >= 4)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                evalOnce();
            }
            const size_t compiledEagerly = cache.GetStats().workerCompiles - compilesBefore;
            // With every combination ready, switching renders specialised at once.
            specNode = graph.FindNode(nodeId);
            ShaderLab::Graph::SetNodeProperty(*specNode, L"Mode", 2.0f); ShaderLab::Graph::MarkNodeChanged(*specNode);
            for (int i = 0; i < 2; ++i) evalOnce();
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, 8, 8, 1, 1, g_dc.get());
            CaseEvaluator().SetAsyncCompile(false);
            const bool firstFrameSpecialised = readback.status == ShaderLab::Rendering::ReadPixelRegionStatus::Success &&
                readback.pixels.size() >= 4 && std::abs(readback.pixels[3] - 0.5f) < 2e-3f && std::abs(readback.pixels[0] - 0.2f) < 2e-3f;
            printf("  [info] async: %zu background compiles while showing only Mode 0 (peak %u seen compiling), %.0f ms\n",
                   compiledEagerly, peakCompiling,
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            TEST("OptSpec_AsyncQueuesEveryCombination", compiledEagerly >= 4);   // baseline + 3 variants
            TEST("OptSpec_AsyncAllVariantsFinish", specNode->variantsCompiling == 0 && !specNode->compilePending);
            TEST("OptSpec_AsyncSwitchIsImmediatelySpecialised", firstFrameSpecialised);
        }
        CaseEvaluator().ReleaseCache();
    }

    // The evaluator's per-node shader and variant state follows what is
    // actually loaded, through in-place updates that keep the shader GUID.
    SL_TEST_CASE(TestVariantReloadState, Tag::Gpu)
    {
        printf("\n=== Variant reload state ===\n");
        using namespace ShaderLab::Effects;
        using float3v = winrt::Windows::Foundation::Numerics::float3;
        auto& registry = ShaderLabEffects::Instance();
        static const std::wstring scHlsl = LR"HLSL(
#include "shaderlab_params.hlsli"
cbuffer Constants : register(b0)
{
    SHADERLAB_OPTION(uint, Mode)   // 0 Red, 1 Green, 2 Custom
    float3 CustomColor;
    float  Gain;
};
SHADERLAB_OPTION_VALUE(uint, Mode)
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 c = Source.Sample(InputSampler, uv0.xy);
    float3 col = (Mode == 0) ? float3(1, 0, 0) : (Mode == 1) ? float3(0, 1, 0) : CustomColor;
    return float4(col * Gain + c.rgb * 0.0, _SLOPT_Mode_MODE ? 0.5 : 1.0);
}
)HLSL";
        // Same length, so an edit between them keeps the source length.
        static const std::wstring scGreen = L"float3(0, 1, 0)";
        static const std::wstring scBlue  = L"float3(0, 0, 1)";
        auto swapColour = [](std::wstring source, const std::wstring& from, const std::wstring& to) {
            const auto at = source.find(from);
            if (at != std::wstring::npos) source.replace(at, from.size(), to);
            return source;
        };
        // The generic build, compiled here as an editor does before updating the graph.
        auto compileBaseline = [](const std::wstring& source) {
            std::string hlsl(source.begin(), source.end());
            std::vector<ShaderCompiler::MacroDef> macros{ { "_SLOPT_Mode_MODE", "0" }, { "_SLOPT_Mode", "0" } };
            auto result = ShaderCompiler::CompileFromString(hlsl, "VariantReloadTest", "main", "ps_5_0", macros);
            std::vector<uint8_t> bytes;
            if (result.succeeded && result.bytecode)
            {
                const auto* data = static_cast<const uint8_t*>(result.bytecode->GetBufferPointer());
                bytes.assign(data, data + result.bytecode->GetBufferSize());
            }
            return bytes;
        };

        // Shader GUIDs follow the bytecode
        {
            GUID definitionGuid{};
            CoCreateGuid(&definitionGuid);
            const auto green = compileBaseline(scHlsl);
            const auto blue  = compileBaseline(swapColour(scHlsl, scGreen, scBlue));
            using ShaderLab::Rendering::GraphEvaluator;
            TEST("VariantReload_ShaderGuidFollowsBytecode", !green.empty() && !blue.empty() &&
                 IsEqualGUID(GraphEvaluator::ShaderGuidFor(definitionGuid, green),
                             GraphEvaluator::ShaderGuidFor(definitionGuid, green)) &&
                 !IsEqualGUID(GraphEvaluator::ShaderGuidFor(definitionGuid, green),
                              GraphEvaluator::ShaderGuidFor(definitionGuid, blue)));
        }

        CaseEvaluator().ReleaseCache();
        ShaderLab::Graph::GraphModel graph;
        auto source = graph.AddNode(ShaderLabEffects::CreateNode(*registry.FindByName(L"Gamut Source")));
        auto node = ShaderLabEffects::CreateNode(*registry.FindByName(L"ICtCp Round-Trip Validator"));
        {
            auto& def = node.customEffect.value();
            def.hlslSource = scHlsl;
            def.compiledBytecode.clear();
            def.shaderLabEffectId = L"Test Variant Reload";
            def.parameters.clear();
            ShaderLab::Graph::ParameterDefinition mode{ L"Mode", L"float", 0.0f, 0.0f, 2.0f, 1.0f,
                                                        { L"Red", L"Green", L"Custom" } };
            mode.specialize = true;
            def.parameters.push_back(mode);
            def.parameters.push_back({ L"CustomColor", L"float3", float3v{ 0.2f, 0.4f, 0.6f }, 0.0f, 1.0f, 0.01f });
            def.parameters.push_back({ L"Gain", L"float", 1.0f, 0.0f, 4.0f, 0.01f });
        }
        node.name = L"Test Variant Reload";
        node.properties.clear();
        ShaderLab::Graph::SetNodeProperty(node, L"Mode", 0.0f);
        ShaderLab::Graph::SetNodeProperty(node, L"CustomColor", float3v{ 0.2f, 0.4f, 0.6f });
        ShaderLab::Graph::SetNodeProperty(node, L"Gain", 1.0f);
        const auto nodeId = graph.AddNode(std::move(node));
        graph.Connect(source, 0, nodeId, 0);

        auto evalOnce = [&] { g_dc->SetTarget(nullptr); CaseEvaluator().Evaluate(graph, g_dc.get()); };
        auto render = [&](float modeValue) -> std::array<float, 4> {
            auto* reloadNode = graph.FindNode(nodeId);
            ShaderLab::Graph::SetNodeProperty(*reloadNode, L"Mode", modeValue);
            ShaderLab::Graph::MarkNodeChanged(*reloadNode);
            evalOnce();
            evalOnce();
            auto readback = ShaderLab::Rendering::ReadPixelRegion(graph, nodeId, 8, 8, 1, 1, g_dc.get());
            if (readback.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || readback.pixels.size() < 4)
                return { -1, -1, -1, -1 };
            return { readback.pixels[0], readback.pixels[1], readback.pixels[2], readback.pixels[3] };
        };
        auto near4 = [](const std::array<float, 4>& actual, float expectR, float expectG, float expectB, float expectA) {
            return std::abs(actual[0] - expectR) < 2e-3f && std::abs(actual[1] - expectG) < 2e-3f &&
                   std::abs(actual[2] - expectB) < 2e-3f && std::abs(actual[3] - expectA) < 2e-3f;
        };
        // An in-place update with a new baseline, keeping the shader GUID.
        auto updateInPlace = [&](const std::wstring& from, const std::wstring& to) {
            auto* reloadNode = graph.FindNode(nodeId);
            auto& reloadDef = reloadNode->customEffect.value();
            reloadDef.hlslSource = swapColour(reloadDef.hlslSource, from, to);
            reloadDef.compiledBytecode = compileBaseline(reloadDef.hlslSource);
            CaseEvaluator().UpdateNodeShader(nodeId, *reloadNode);
            ShaderLab::Graph::MarkNodeChanged(*reloadNode);
        };

        // UpdateNodeShader loads the baseline; the same variant next frame reloads
        auto variantBefore = render(0.0f);
        CaseEvaluator().UpdateNodeShader(nodeId, *graph.FindNode(nodeId));
        auto variantAfter = render(0.0f);
        printf("  [info] variant before update a%.2f, after a%.2f (0.5 = specialised)\n", variantBefore[3], variantAfter[3]);
        TEST("VariantReload_UpdateNodeShaderReloadsVariant",
             near4(variantBefore, 1, 0, 0, 0.5f) && near4(variantAfter, 1, 0, 0, 0.5f));

        // New bytecode under the same definition GUID renders
        auto genericGreen = render(1.4f);
        updateInPlace(scGreen, scBlue);
        auto genericBlue = render(1.4f);
        auto variantBlue = render(1.0f);
        printf("  [info] same GUID: before %.2f %.2f %.2f | after %.2f %.2f %.2f | variant %.2f %.2f %.2f a%.2f\n",
               genericGreen[0], genericGreen[1], genericGreen[2], genericBlue[0], genericBlue[1], genericBlue[2],
               variantBlue[0], variantBlue[1], variantBlue[2], variantBlue[3]);
        TEST("VariantReload_SameGuidNewBytecodeRenders",
             near4(genericGreen, 0, 1, 0, 1.0f) && near4(genericBlue, 0, 0, 1, 1.0f) && near4(variantBlue, 0, 0, 1, 0.5f));

        // compilePending clears when the node stops waiting on a compile
        CaseEvaluator().SetAsyncCompile(true);
        auto& cache = BytecodeCache::Instance();
        auto restartCompile = [&] {
            auto* reloadNode = graph.FindNode(nodeId);
            reloadNode->customEffect->hlslSource += L"\n// variant reload test " +
                std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"\n";
            reloadNode->customEffect->compiledBytecode.clear();
            ShaderLab::Graph::MarkNodeChanged(*reloadNode);
            evalOnce();
            return graph.FindNode(nodeId)->compilePending;
        };
        const bool pendingBeforeUnneeded = restartCompile();
        graph.FindNode(nodeId)->needed = false;
        evalOnce();
        const bool clearedWhenUnneeded = !graph.FindNode(nodeId)->compilePending;
        graph.FindNode(nodeId)->needed = true;
        TEST("VariantReload_UnneededNodeClearsCompilePending", pendingBeforeUnneeded && clearedWhenUnneeded);

        const bool pendingBeforeReplace = restartCompile();
        {
            auto* reloadNode = graph.FindNode(nodeId);
            reloadNode->customEffect->compiledBytecode = compileBaseline(reloadNode->customEffect->hlslSource);
            CoCreateGuid(&reloadNode->customEffect->shaderGuid);
            ShaderLab::Graph::MarkNodeChanged(*reloadNode);
        }
        evalOnce();
        TEST("VariantReload_PrecompiledDefinitionClearsCompilePending",
             pendingBeforeReplace && !graph.FindNode(nodeId)->compilePending);

        // A same-length edit under the same GUID queues the new variants
        auto waitForIdleCache = [&] {
            const auto start = std::chrono::steady_clock::now();
            while (cache.GetStats().pendingCount > 0 && std::chrono::steady_clock::now() - start < std::chrono::seconds(60))
            {
                evalOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            evalOnce();
        };
        render(0.0f);
        waitForIdleCache();
        updateInPlace(scBlue, scGreen);
        const auto compilesBefore = cache.GetStats().workerCompiles;
        {
            const auto start = std::chrono::steady_clock::now();
            evalOnce();
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(30))
            {
                if (cache.GetStats().workerCompiles - compilesBefore >= 3 && graph.FindNode(nodeId)->variantsCompiling == 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                evalOnce();
            }
        }
        const size_t requeued = cache.GetStats().workerCompiles - compilesBefore;
        auto switched = render(1.0f);
        printf("  [info] same-length edit: %zu variants compiled, Mode 1 -> %.2f %.2f %.2f a%.2f\n",
               requeued, switched[0], switched[1], switched[2], switched[3]);
        TEST("VariantReload_SameLengthEditQueuesVariants", requeued >= 3 && near4(switched, 0, 1, 0, 0.5f));

        // A definition with no options leaves no stale variant count
        {
            auto* reloadNode = graph.FindNode(nodeId);
            reloadNode->customEffect->parameters[0].specialize = false;
            updateInPlace(scGreen, scBlue);
            reloadNode->variantsCompiling = 7;
        }
        evalOnce();
        TEST("VariantReload_NoOptionsClearsVariantCount", graph.FindNode(nodeId)->variantsCompiling == 0);
        graph.FindNode(nodeId)->variantsCompiling = 3;
        CaseEvaluator().ReleaseCache(graph);
        TEST("VariantReload_ReleaseCacheClearsVariantCount", graph.FindNode(nodeId)->variantsCompiling == 0);
        CaseEvaluator().SetAsyncCompile(false);
        CaseEvaluator().ReleaseCache();
    }
}
