#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/BytecodeCache.h"
#include "Effects/ShaderCompiler.h"
#include "Effects/ShaderLabParamsHlsl.h"
#include "Effects/ShaderVariants.h"
#include "Graph/GraphModel.h"
#include "Rendering/D3D11ComputeRunner.h"
#include "Rendering/GraphEvaluator.h"

#include <cmath>
#include <cstdio>
#include <future>
#include <string>
#include <vector>

// Shader compilation, the shared HLSL includes and every built-in in each
// binding build.
namespace
{
    using namespace ShaderLab::Tests;

    SL_TEST_CASE(TestShaderCompilation, Tag::Fast)
    {
        printf("\n=== Shader Compilation ===\n");

        // NOTE: the inline HLSL in this test is a COMPILE/CACHE-KEY fixture,
        // not a signature example. It is never rasterised, so its
        // two-parameter main() is harmless here -- but do not copy it. The
        // real contract is (SV_POSITION, SCENE_POSITION, TEXCOORD0..N), one
        // TEXCOORD per input; TestPixelShaderSignatures enforces it over the
        // shipped catalog.
        std::string validPS = R"(
Texture2D Source : register(t0);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{ return Source.Load(int3(uv0.xy, 0)); }
)";
        auto result = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            validPS, "test.hlsl", "main", "ps_5_0");
        TEST("ValidPixelShader", result.succeeded);

        auto bad = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            "not hlsl!", "bad.hlsl", "main", "ps_5_0");
        TEST("InvalidShaderFails", !bad.succeeded);

        std::string validCS = R"(
RWTexture2D<float4> output : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) { output[id.xy] = float4(1,0,0,1); }
)";
        auto csResult = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            validCS, "test.hlsl", "main", "cs_5_0");
        TEST("ValidComputeShader", csResult.succeeded);
    }

    // Lane 3: analysis values as a GPU-resident texture. A compute-analysis
    // node publishes Result[] three ways; lane 3 is the
    // 1 x N RGBA32F texture, the only GPU-resident route a Direct2D PIXEL
    // shader can consume. D2D maps effect INPUTS to t0, t1, ... and will
    // not bind an arbitrary SRV, and CreateResourceTexture takes CPU bytes
    // -- so it would reintroduce the readback the lane exists to remove.
    //
    // Two properties worth pinning: the lane is OPT-IN (no request means
    // no texture, no copy shader, no dispatch), and when requested the
    // texels match the structured buffer exactly. A copy that quietly
    // reordered or dropped a field would surface far downstream as a
    // shader reading the wrong statistic.
    SL_TEST_CASE(TestComputeRunnerAnalysisTexture, Tag::Gpu)
    {
        printf("\n=== Compute runner analysis texture ===\n");

        {
            using namespace ShaderLab::Rendering;
            D3D11ComputeRunner runner;
            runner.Initialize(g_d3dDevice.get());

            const char* kProducer = R"(
RWStructuredBuffer<float4> Result : register(u0);
cbuffer Constants : register(b0) { uint Width; uint Height; };
[numthreads(1,1,1)]
void main(uint3 t : SV_DispatchThreadID) {
    Result[0] = float4(11, 0, 0, 0);
    Result[1] = float4(22, 0, 0, 0);
    Result[2] = float4(33, 0, 0, 0);
    Result[3] = float4(44, 0, 0, 0);
}
)";
            bool compiled = runner.CompileShader(kProducer);
            TEST("Lane3_ProducerCompiles", compiled);

            if (compiled)
            {
                TEST("Lane3_OptInByDefault", !runner.AnalysisTextureRequested());
                winrt::com_ptr<ID3D11Texture2D> none;
                TEST("Lane3_NoTextureBeforeRequest",
                    runner.GetAnalysisTexture(none.put()) == E_NOT_VALID_STATE);

                TEST("Lane3_RequestSucceeds", runner.RequestAnalysisTexture() == S_OK);

                // Dispatch needs a real input at t0 -- it reads Width/Height
                // off it to auto-inject the constants, and bails on null.
                D3D11_TEXTURE2D_DESC id{};
                id.Width = 4; id.Height = 4; id.MipLevels = 1; id.ArraySize = 1;
                id.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                id.SampleDesc.Count = 1;
                id.Usage = D3D11_USAGE_DEFAULT;
                id.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                winrt::com_ptr<ID3D11Texture2D> inTex;
                g_d3dDevice->CreateTexture2D(&id, nullptr, inTex.put());

                // The readback here belongs to the TEST, not to the lane --
                // lane 3 itself never touches the CPU.
                std::vector<float> vals = runner.Dispatch(inTex.get(), {}, 4);
                TEST("Lane3_DispatchProducedFields", vals.size() >= 16);

                winrt::com_ptr<ID3D11Texture2D> tex;
                HRESULT hrTex = runner.GetAnalysisTexture(tex.put());
                TEST("Lane3_TextureAvailableAfterDispatch", hrTex == S_OK && tex);

                bool shapeOk = false, texelsOk = false;
                if (tex)
                {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    shapeOk = (td.Height == 1 && td.Width == 4 &&
                               td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT);

                    D3D11_TEXTURE2D_DESC sd = td;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.BindFlags = 0;
                    sd.MiscFlags = 0;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    winrt::com_ptr<ID3D11Texture2D> staging;
                    if (SUCCEEDED(g_d3dDevice->CreateTexture2D(&sd, nullptr, staging.put())))
                    {
                        g_d3dContext->CopyResource(staging.get(), tex.get());
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (SUCCEEDED(g_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)))
                        {
                            auto* row = static_cast<const float*>(m.pData);
                            texelsOk = (row[0] == 11.0f && row[4] == 22.0f &&
                                        row[8] == 33.0f && row[12] == 44.0f);
                            g_d3dContext->Unmap(staging.get(), 0);
                        }
                    }
                }
                TEST("Lane3_TextureShape (1 row, one texel per float4 slot)", shapeOk);
                TEST("Lane3_TexelsMatchStructuredBuffer", texelsOk);

                // Lane 3 must not have disturbed lane 1 (SRV) or lane 2 (CPU).
                TEST("Lane3_DoesNotDisturbCpuValues",
                    vals.size() >= 16 && vals[0] == 11.0f && vals[12] == 44.0f);
                TEST("Lane3_DoesNotDisturbSrv", runner.GetResultSRV() != nullptr);
            }
        }
    }

    // A shader that uses SHADERLAB_PARAM compiles in
    // both cbuffer mode (_SLPARAM_X_GPU=0) and GPU-bound mode
    // (_SLPARAM_X_GPU=1). We don't reflect the resulting bytecode
    // here -- the goal is just to prove the include resolver +
    // macro injection wire all the way through D3DCompile without
    // a syntax error in the macro file itself.
    SL_TEST_CASE(TestParamsHlsliModes, Tag::Fast)
    {
        printf("\n=== shaderlab_params.hlsli modes ===\n");

        std::string macroPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
SHADERLAB_GPU_BUFFER(Exposure, t1)
cbuffer Constants : register(b0) {
    SHADERLAB_PARAM(float, Exposure)
    float Padding;
};
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    SHADERLAB_LOAD_PARAM(float, Exposure)
    return Source.Load(int3(uv0.xy, 0)) * Exposure;
}
)";
        // Default mode: parameter lives in cbuffer.
        auto cbufferMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_cbuffer.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "0" } });
        TEST("ShaderLabParamsHlsli_CbufferMode", cbufferMode.succeeded);

        // GPU-bound mode: parameter comes from StructuredBuffer<float4>.
        auto gpuMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_gpu.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "1" } });
        TEST("ShaderLabParamsHlsli_GpuMode", gpuMode.succeeded);

        // Texture-smuggled mode: the value arrives as a Texture2D effect
        // INPUT rather than a StructuredBuffer. This is the only GPU-resident
        // route available to a Direct2D PIXEL shader -- D2D maps a transform's
        // inputs to t0, t1, ... and will not bind an arbitrary SRV, while
        // ID2D1EffectContext::CreateResourceTexture takes `const BYTE*` CPU
        // memory and so would reintroduce the very readback the binding
        // exists to remove. A ps_5_0 compile is the meaningful check: the
        // StructuredBuffer form above compiles for cs_5_0 too, so it cannot
        // prove the pixel-shader path works.
        auto texMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
            macroPS, "test_param_tex.hlsl", "main", "ps_5_0",
            { { "_SLPARAM_Exposure_GPU", "2" } });
        TEST("ShaderLabParamsHlsli_TextureMode (pixel-shader GPU binding)",
            texMode.succeeded);

        // All three modes must compile from the SAME source text. If an
        // author had to write the shader differently per mode, the host could
        // not switch binding strategy without re-authoring the effect.
        TEST("ShaderLabParamsHlsli_AllModesShareOneSource",
            cbufferMode.succeeded && gpuMode.succeeded && texMode.succeeded);

        // The direct-access helpers must also compile in a pixel shader --
        // they are the escape hatch for an effect that wants a whole analysis
        // row rather than one parameter at a time.
        {
            std::string helperPS = R"(
#include "shaderlab_params.hlsli"
Texture2D Source : register(t0);
Texture2D<float4> Analysis : register(t1);
float4 main(float4 pos : SV_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET {
    float  maxNits = ShaderLabAnalysisField(Analysis, 1);
    float4 row     = ShaderLabAnalysisTexel(Analysis, 7);
    return Source.Load(int3(uv0.xy, 0)) * maxNits + row;
}
)";
            auto helperMode = ShaderLab::Effects::ShaderCompiler::CompileFromString(
                helperPS, "test_analysis_helpers.hlsl", "main", "ps_5_0", {});
            TEST("ShaderLabParamsHlsli_AnalysisHelpers", helperMode.succeeded);
        }
    }

    // ICtCp Gamut Boundary LUT: a generator the host can drive.
    SL_TEST_CASE(TestGamutLutContract, Tag::Fast)
    {
        printf("\n=== Gamut Boundary LUT contract ===\n");

        {
            using namespace ShaderLab::Effects;
            const auto* desc = ShaderLabEffects::Instance().FindById(L"ICtCp Gamut Boundary LUT");
            TEST("GamutLut_DescriptorExists", desc != nullptr);
            if (desc)
            {
                // A generator: no image inputs, an image output, and a
                // dispatch the evaluator tiles over the output.
                TEST("GamutLut_IsGenerator", desc->inputNames.empty() && desc->hasImageOutput);
                TEST("GamutLut_TiledDispatch",
                     desc->dispatchShape == ShaderLab::Graph::DispatchShape::PerOutputPixel &&
                     desc->hlslSource.find("[numthreads(8, 8, 1)]") != std::string::npos);

                // The size is fixed because the reader indexes with the same
                // constants; it has to be in hiddenDefaults for the evaluator's
                // explicit-size path to allocate the output at all.
                auto dim = [&](const wchar_t* key) -> uint32_t {
                    auto it = desc->hiddenDefaults.find(key);
                    if (it == desc->hiddenDefaults.end()) return 0;
                    if (auto* u = std::get_if<uint32_t>(&it->second)) return *u;
                    return 0;
                };
                const uint32_t w = dim(L"OutputWidth"), h = dim(L"OutputHeight");
                TEST("GamutLut_FixedSizeHidden", w > 0 && h > 1);
                TEST("GamutLut_WidthIsPowerOfTwo", w > 0 && (w & (w - 1)) == 0);

                std::string src(desc->hlslSource.begin(), desc->hlslSource.end());
                const std::string& gamutText = GetGamutHLSL();
                // Geometry in the shared header must agree with the hidden size.
                TEST("GamutLut_HlslGeometryMatches",
                     gamutText.find("#define GAMUT_LUT_W " + std::to_string(w)) != std::string::npos &&
                     gamutText.find("#define GAMUT_LUT_H " + std::to_string(h)) != std::string::npos);
                // The generator and every reader take the search from the shared header.
                TEST("GamutLut_SharesBoundarySearch",
                     src.find(std::string("#include \"") + cGamutIncludeName + "\"") != std::string::npos &&
                     gamutText.find("float CubeBoundaryRadius(") != std::string::npos);
                // The stamp carries the table width, so a reader built for
                // another width rejects the table instead of misindexing it.
                TEST("GamutLut_StampCarriesWidthTag",
                     gamutText.find("#define GAMUT_LUT_STAMP_OFFSET (GAMUT_LUT_W * 0.5)") != std::string::npos &&
                     gamutText.find("t.fingerprint + GAMUT_LUT_STAMP_OFFSET") != std::string::npos &&
                     src.find("GamutLutStampAlpha(tg)") != std::string::npos);

                auto compiled = ShaderCompiler::CompileFromString(
                    src, "gamut_lut.hlsl", "main", "cs_5_0", {});
                if (!compiled.succeeded && compiled.errors)
                    printf("  [info] compile errors: %.900s\n",
                        static_cast<const char*>(compiled.errors->GetBufferPointer()));
                TEST("GamutLut_Compiles", compiled.succeeded);
                if (compiled.succeeded)
                {
                    auto refl = ShaderCompiler::Reflect(compiled.bytecode.get());
                    auto has = [&](const wchar_t* n) {
                        for (const auto& cb : refl.constantBuffers)
                            for (const auto& v : cb.variables)
                                if (v.name == n) return true;
                        return false;
                    };
                    // The host packs these by name: the runner's auto-injected
                    // source dims, the hidden size, and the one real parameter.
                    TEST("GamutLut_CbufferHasOutputDims", has(L"OutputWidth") && has(L"OutputHeight"));
                    TEST("GamutLut_CbufferHasSdrWhiteNits", has(L"SdrWhiteNits"));
                }
            }
        }
    }

    // The Soft Compress params are the first cbuffer payload past byte
    // 64 in any ShaderLab effect. Compile the real registry descriptor
    // exactly as graph-load does and assert the reflected layout, so a
    // packing/reflection size bug can't silently zero them again.
    SL_TEST_CASE(TestGamutMapCbufferLayout, Tag::Fast)
    {
        printf("\n=== ICtCp Gamut Map cbuffer layout ===\n");

        {
            using namespace ShaderLab::Effects;
            const auto* desc = ShaderLabEffects::Instance().FindById(L"ICtCp Gamut Map");
            TEST("ICtCpGamutMap_DescriptorExists", desc != nullptr);
            if (desc)
            {
                std::string src(desc->hlslSource.begin(), desc->hlslSource.end());
                auto compiled = ShaderCompiler::CompileFromString(
                    src, "ictcp_gamut_map.hlsl", "main", "ps_5_0");
                TEST("ICtCpGamutMap_Compiles", compiled.succeeded);
                if (compiled.succeeded)
                {
                    auto refl = ShaderCompiler::Reflect(compiled.bytecode.get());
                    TEST("ICtCpGamutMap_HasCbuffer", !refl.constantBuffers.empty());
                    if (!refl.constantBuffers.empty())
                    {
                        const auto& cb = refl.constantBuffers[0];
                        printf("  [info] cbuffer '%ls' sizeBytes=%u vars=%zu\n",
                            cb.name.c_str(), cb.sizeBytes, cb.variables.size());
                        auto findVar = [&](const wchar_t* n) -> const ShaderVariable* {
                            for (const auto& v : cb.variables)
                                if (v.name == n) return &v;
                            return nullptr;
                        };
                        const auto* st = findVar(L"SoftThreshold");
                        const auto* sl = findVar(L"SoftLimit");
                        const auto* kh = findVar(L"KneeHardness");
                        for (const auto& v : cb.variables)
                            printf("  [info] var %ls offset=%u size=%u\n",
                                v.name.c_str(), v.offset, v.size);
                        // The parameter block still ends at byte 80; the two
                        // host-built tables (derived constants) follow it:
                        // 64 + 1536 float4.
                        const auto* fit  = findVar(L"FitScaleLut");
                        const auto* poly = findVar(L"TgtPolyLut");
                        TEST("ICtCpGamutMap_FitScaleLutAt80", fit && fit->offset == 80 && fit->size == 64 * 16);
                        TEST("ICtCpGamutMap_TgtPolyLutFollows", poly && poly->offset == 80 + 64 * 16 && poly->size == 1536 * 16);
                        TEST("ICtCpGamutMap_CbufferSize", cb.sizeBytes == 80 + (64 + 1536) * 16);
                        TEST("ICtCpGamutMap_SoftThresholdAt64", st && st->offset == 64);
                        TEST("ICtCpGamutMap_SoftLimitAt68",     sl && sl->offset == 68);
                        TEST("ICtCpGamutMap_KneeHardnessAt72",  kh && kh->offset == 72);
                    }
                }
            }
        }
    }

    // The library without its include guard, as graphs saved before the
    // include existed carry it inline.
    std::string LegacyColorMathText()
    {
        std::string text = ShaderLab::Effects::GetColorMathHLSL();
        const std::string guardOpen =
            "#ifndef SHADERLAB_COLORMATH_HLSLI_INCLUDED\n#define SHADERLAB_COLORMATH_HLSLI_INCLUDED\n";
        const std::string guardClose = "#endif\n";
        const size_t openPos = text.find(guardOpen);
        if (openPos != std::string::npos) text.erase(openPos, guardOpen.size());
        const size_t closePos = text.rfind(guardClose);
        if (closePos != std::string::npos && closePos + guardClose.size() == text.size())
            text.erase(closePos);
        return text;
    }

    // Bytecode for `source`, empty when it fails to compile.
    std::vector<uint8_t> CompileToBytes(const std::string& source, const std::string& target,
                                        const std::vector<std::pair<std::string, std::string>>& macroValues)
    {
        using ShaderLab::Effects::ShaderCompiler;
        std::vector<ShaderCompiler::MacroDef> macros;
        for (const auto& [name, value] : macroValues) macros.push_back({ name.c_str(), value.c_str() });
        auto result = ShaderCompiler::CompileFromString(source, "ColorMathTest", "main", target, macros);
        if (!result.succeeded || !result.bytecode) return {};
        const auto* bytes = static_cast<const uint8_t*>(result.bytecode->GetBufferPointer());
        return std::vector<uint8_t>(bytes, bytes + result.bytecode->GetBufferSize());
    }

    SL_TEST_CASE(TestColorMathInclude, Tag::Gpu)
    {
        printf("\n=== Color Math Include ===\n");
        using namespace ShaderLab::Effects;
        const std::string includeLine = std::string("#include \"") + cColorMathIncludeName + "\"\n";

        // A small shader through the include, twice, and after a prepended copy.
        const std::string body = R"(
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Sample(InputSampler, uv0.xy);
    return float4(ScRGBToICtCp(color.rgb), color.a);
}
)";
        const auto included = CompileToBytes(includeLine + body, "ps_5_0", {});
        TEST("ColorMathHlsli_IncludeCompiles", !included.empty());
        TEST("ColorMathHlsli_IncludeTwiceCompiles",
             !CompileToBytes(includeLine + includeLine + body, "ps_5_0", {}).empty());
        TEST("ColorMathHlsli_IncludeAfterPrependCompiles",
             !CompileToBytes(GetColorMathHLSL() + includeLine + body, "ps_5_0", {}).empty());
        const auto prepended = CompileToBytes(LegacyColorMathText() + body, "ps_5_0", {});
        TEST("ColorMathHlsli_MatchesPrependedForm", !included.empty() && included == prepended);

        // Every built-in that includes the library compiles to the same bytes
        // as the prepended source it replaced: the generic build, plus one
        // fully specialised variant where the effect has options.
        struct BuiltinCheck { std::wstring name; size_t variants{ 0 }; size_t mismatches{ 0 }; };
        std::vector<std::future<BuiltinCheck>> checks;
        for (const auto& descriptor : ShaderLabEffects::Instance().All())
        {
            if (descriptor.IsUserEffect()) continue;
            const size_t includePos = descriptor.hlslSource.find(includeLine);
            if (includePos == std::string::npos) continue;
            checks.push_back(std::async(std::launch::async, [&descriptor, includePos, &includeLine]
            {
                std::string legacySource = descriptor.hlslSource;
                legacySource.erase(includePos, includeLine.size());
                legacySource = LegacyColorMathText() + legacySource;

                const auto node = ShaderLabEffects::CreateNode(descriptor);
                const auto& definition = *node.customEffect;
                const std::string target =
                    definition.shaderType == ShaderLab::Graph::CustomShaderType::PixelShader ? "ps_5_0" : "cs_5_0";
                std::vector<std::vector<std::pair<std::string, std::string>>> variants{ GenericVariantMacros(definition) };
                const auto optionKeys = AllOptionKeys(definition);
                if (!optionKeys.empty())
                {
                    auto macros = GenericVariantMacros(definition);
                    const auto optionNames = SpecializedOptionNames(definition);
                    for (size_t index = 0; index < optionNames.size(); ++index)
                    {
                        const uint32_t slot = OptionKeySlot(optionKeys.back(), index);
                        for (auto& [name, value] : macros)
                        {
                            if (name == "_SLOPT_" + optionNames[index] + "_MODE") value = slot ? "1" : "0";
                            else if (name == "_SLOPT_" + optionNames[index]) value = std::to_string(slot ? slot - 1u : 0u);
                        }
                    }
                    variants.push_back(std::move(macros));
                }

                BuiltinCheck check{ descriptor.name };
                for (const auto& macros : variants)
                {
                    const auto newBytes = CompileToBytes(descriptor.hlslSource, target, macros);
                    const auto oldBytes = CompileToBytes(legacySource, target, macros);
                    ++check.variants;
                    // Compare outcomes; whether a built-in compiles at all is not this test's concern.
                    if (newBytes.empty() && oldBytes.empty())
                        printf("  [info] %ls does not compile in either form\n", descriptor.name.c_str());
                    else if (newBytes != oldBytes)
                    {
                        ++check.mismatches;
                        printf("  [info] %ls: include form %zu bytes, prepended form %zu bytes\n",
                               descriptor.name.c_str(), newBytes.size(), oldBytes.size());
                    }
                }
                return check;
            }));
        }
        size_t builtinCount = 0, variantCount = 0, mismatchCount = 0;
        for (auto& pending : checks)
        {
            const auto check = pending.get();
            ++builtinCount;
            variantCount += check.variants;
            mismatchCount += check.mismatches;
        }
        printf("  [info] %zu built-ins include the library; %zu builds compared\n", builtinCount, variantCount);
        TEST("ColorMathHlsli_BuiltinsUseInclude", builtinCount >= 20);
        TEST("ColorMathHlsli_BuiltinsMatchPrependedForm", builtinCount > 0 && mismatchCount == 0);

        // A shader with neither include, carrying its own math and a plain
        // cbuffer, compiles through every path.
        const std::string standalonePixel = R"(
float MyPQ(float n) { float p = pow(max(n, 0.0), 1.0 / 78.84375); return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125); }
cbuffer Constants : register(b0) { float Exposure; };
Texture2D Source : register(t0);
SamplerState InputSampler : register(s0);
float4 main(float4 pos : SV_POSITION, float4 scenePos : SCENE_POSITION, float4 uv0 : TEXCOORD0) : SV_TARGET
{
    float4 color = Source.Sample(InputSampler, uv0.xy);
    return float4(color.rgb * Exposure * MyPQ(0.5) / 80.0, color.a);
}
)";
        TEST("NoIncludes_CompileFromString", !CompileToBytes(standalonePixel, "ps_5_0", {}).empty());
        {
            ShaderLab::Graph::CustomEffectDefinition definition;
            definition.parameters.push_back({ L"Exposure", L"float", 1.0f, 0.0f, 4.0f, 0.01f });
            TEST("NoIncludes_GenericVariantMacros",
                 !CompileToBytes(standalonePixel, "ps_5_0", GenericVariantMacros(definition)).empty());
        }
        {
            auto& cache = BytecodeCache::Instance();
            const std::string canonical = CanonicalizeHlslSource(standalonePixel);
            BytecodeCompileRequest request;
            request.key.sourceHash         = HashCanonicalSource(canonical);
            request.key.paramSignatureHash = HashParamSignature({});
            request.key.includeLibraryHash = IncludeLibraryHash();
            request.key.entryPoint         = "main";
            request.key.target             = "ps_5_0";
            request.metadata.effectId      = L"NoIncludesTest";
            request.metadata.version       = 1;
            request.hlslSource             = canonical;
            TEST("NoIncludes_BytecodeCache", cache.GetOrCompile(request).status == BytecodeStatus::Ready);
        }
        const std::string standaloneCompute = R"(
float MyLuma(float3 rgb) { return dot(rgb, float3(0.2126, 0.7152, 0.0722)); }
RWStructuredBuffer<float4> Result : register(u0);
cbuffer Constants : register(b0) { uint Width; uint Height; float Exposure; };
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { Result[0] = float4(MyLuma(float3(1, 1, 1)) * Exposure, Width, Height, 0); }
)";
        {
            ShaderLab::Rendering::D3D11ComputeRunner runner;
            runner.Initialize(g_d3dDevice.get());
            TEST("NoIncludes_D3D11ComputeRunner", runner.CompileShader(standaloneCompute));
            // A graph saved with the library inline still compiles beside the params header.
            TEST("InlineLibrary_D3D11ComputeRunner",
                 runner.CompileShader(LegacyColorMathText() + "#include \"shaderlab_params.hlsli\"\n" + standaloneCompute));
            TEST("ColorMathHlsli_D3D11ComputeRunner", runner.CompileShader(includeLine + standaloneCompute));
        }
        TEST("InlineLibrary_CompileFromString",
             !CompileToBytes(LegacyColorMathText() + "#include \"shaderlab_params.hlsli\"\n" + body, "ps_5_0", {}).empty());

        // The cache key covers the header texts, so editing one invalidates cached bytecode.
        const std::string_view paramsText(GetShaderLabParamsHLSL(), GetShaderLabParamsHLSLLength());
        const std::string& colorMathText = GetColorMathHLSL();
        const std::string& gamutText = GetGamutHLSL();
        TEST("IncludeLibraryHash_CoversAllHeaders",
             IncludeLibraryHash() == IncludeLibraryHashOf(paramsText, colorMathText, gamutText));
        TEST("IncludeLibraryHash_ChangesWithColorMath",
             IncludeLibraryHashOf(paramsText, colorMathText, gamutText) !=
             IncludeLibraryHashOf(paramsText, colorMathText + "\n// edit\n", gamutText));
        TEST("IncludeLibraryHash_ChangesWithParams",
             IncludeLibraryHashOf(paramsText, colorMathText, gamutText) !=
             IncludeLibraryHashOf(std::string(paramsText) + "\n// edit\n", colorMathText, gamutText));
    }

    // The gamut library without its guard or its color-math include, as a
    // shader that pastes it in carries it.
    std::string LegacyGamutText()
    {
        std::string text = ShaderLab::Effects::GetGamutHLSL();
        for (const std::string line : { std::string("#ifndef SHADERLAB_GAMUT_HLSLI_INCLUDED\n"),
                                        std::string("#define SHADERLAB_GAMUT_HLSLI_INCLUDED\n"),
                                        std::string("#include \"shaderlab_colormath.hlsli\"\n") })
        {
            const size_t position = text.find(line);
            if (position != std::string::npos) text.erase(position, line.size());
        }
        const std::string guardClose = "#endif\n";
        const size_t closePos = text.rfind(guardClose);
        if (closePos != std::string::npos && closePos + guardClose.size() == text.size())
            text.erase(closePos);
        return text;
    }

    // The generic build, plus the GPU-bound build when the effect has
    // gpu-bindable parameters, plus every fully specialised option variant.
    std::vector<std::vector<std::pair<std::string, std::string>>> BindingVariants(
        const ShaderLab::Graph::CustomEffectDefinition& definition)
    {
        using namespace ShaderLab::Effects;
        const bool pixelShader = definition.shaderType == ShaderLab::Graph::CustomShaderType::PixelShader;
        std::vector<std::vector<std::pair<std::string, std::string>>> variants{ GenericVariantMacros(definition) };
        auto gpuBound = GenericVariantMacros(definition);
        bool hasGpuParams = false;
        for (auto& [name, value] : gpuBound)
        {
            if (name.starts_with("_SLPARAM_"))
            {
                value = pixelShader ? "2" : "1";
                hasGpuParams = true;
            }
        }
        if (hasGpuParams) variants.push_back(std::move(gpuBound));

        const auto optionNames = SpecializedOptionNames(definition);
        for (const uint64_t optionKey : AllOptionKeys(definition))
        {
            auto macros = GenericVariantMacros(definition);
            for (size_t index = 0; index < optionNames.size(); ++index)
            {
                const uint32_t slot = OptionKeySlot(optionKey, index);
                for (auto& [name, value] : macros)
                {
                    if (name == "_SLOPT_" + optionNames[index] + "_MODE") value = slot ? "1" : "0";
                    else if (name == "_SLOPT_" + optionNames[index]) value = std::to_string(slot ? slot - 1u : 0u);
                }
            }
            variants.push_back(std::move(macros));
        }
        return variants;
    }

    SL_TEST_CASE(TestGamutInclude, Tag::Fast)
    {
        printf("\n=== Gamut Include ===\n");
        using namespace ShaderLab::Effects;
        const std::string gamutLine = std::string("#include \"") + cGamutIncludeName + "\"\n";
        const std::string colorMathLine = std::string("#include \"") + cColorMathIncludeName + "\"\n";

        // Exercises the search, the table reader and the stamp check.
        const std::string body = R"(
Texture2D<float4> GamutLut : register(t0);
RWStructuredBuffer<float4> Result : register(u0);
cbuffer Constants : register(b0) { uint Width; uint Height; float Peak; uint Gamut; };
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    TargetXf target = MakeTargetXf(Gamut, float2(0.64, 0.33), float2(0.30, 0.60), float2(0.15, 0.06), D65_WHITE);
    float2 dir = float2(0.6, 0.8);
    float exact = CubeBoundaryRadius(0.3, dir, Peak, target);
    float4 stamp = GamutLut.Load(int3(0, 0, 0));
    float table = GamutLutStampMatches(stamp, target, Peak) ? GamutLutBoundaryRadius(GamutLut, 0.3, dir, stamp.b) : exact;
    Result[0] = float4(exact, table, GamutLutStampAlpha(target), Width + Height);
}
)";
        const auto included = CompileToBytes(gamutLine + body, "cs_5_0", {});
        TEST("GamutHlsli_IncludeCompilesAlone", !included.empty());
        TEST("GamutHlsli_AfterColorMathCompiles",
             !CompileToBytes(colorMathLine + gamutLine + body, "cs_5_0", {}).empty());
        TEST("GamutHlsli_ColorMathTwiceCompiles",
             !CompileToBytes(colorMathLine + gamutLine + colorMathLine + body, "cs_5_0", {}).empty());
        TEST("GamutHlsli_IncludeTwiceCompiles",
             !CompileToBytes(gamutLine + gamutLine + body, "cs_5_0", {}).empty());
        const auto pasted = CompileToBytes(LegacyColorMathText() + LegacyGamutText() + body, "cs_5_0", {});
        TEST("GamutHlsli_MatchesPastedForm", !included.empty() && included == pasted);

        // The table geometry in the header is the size the LUT generator allocates.
        const std::string& gamutText = GetGamutHLSL();
        const auto* lutDescriptor = ShaderLabEffects::Instance().FindById(L"ICtCp Gamut Boundary LUT");
        auto hiddenSize = [lutDescriptor](const wchar_t* key) -> uint32_t
        {
            if (!lutDescriptor) return 0;
            auto it = lutDescriptor->hiddenDefaults.find(key);
            if (it == lutDescriptor->hiddenDefaults.end()) return 0;
            if (auto* value = std::get_if<uint32_t>(&it->second)) return *value;
            return 0;
        };
        const uint32_t lutWidth = hiddenSize(L"OutputWidth"), lutHeight = hiddenSize(L"OutputHeight");
        TEST("GamutHlsli_GeometryMatchesLut",
             lutWidth > 0 && lutHeight > 1 &&
             gamutText.find("#define GAMUT_LUT_W " + std::to_string(lutWidth) + "\n") != std::string::npos &&
             gamutText.find("#define GAMUT_LUT_H " + std::to_string(lutHeight) + "\n") != std::string::npos);

        // Every built-in that includes the header compiles to the same bytes
        // as the same source with the library pasted in.
        size_t builtinCount = 0, buildCount = 0, mismatchCount = 0;
        for (const auto& descriptor : ShaderLabEffects::Instance().All())
        {
            if (descriptor.IsUserEffect()) continue;
            const size_t includePos = descriptor.hlslSource.find(gamutLine);
            if (includePos == std::string::npos) continue;
            ++builtinCount;
            std::string pastedSource = descriptor.hlslSource;
            pastedSource.replace(includePos, gamutLine.size(), LegacyColorMathText() + LegacyGamutText());

            const auto node = ShaderLabEffects::CreateNode(descriptor);
            const auto& definition = *node.customEffect;
            const std::string target =
                definition.shaderType == ShaderLab::Graph::CustomShaderType::PixelShader ? "ps_5_0" : "cs_5_0";
            for (const auto& macros : BindingVariants(definition))
            {
                const auto newBytes = CompileToBytes(descriptor.hlslSource, target, macros);
                const auto oldBytes = CompileToBytes(pastedSource, target, macros);
                ++buildCount;
                if (newBytes.empty() || newBytes != oldBytes)
                {
                    ++mismatchCount;
                    printf("  [info] %ls: include form %zu bytes, pasted form %zu bytes\n",
                           descriptor.name.c_str(), newBytes.size(), oldBytes.size());
                }
            }
        }
        printf("  [info] %zu built-ins include the gamut header; %zu builds compared\n", builtinCount, buildCount);
        TEST("GamutHlsli_LutGeneratorUsesInclude",
             lutDescriptor && lutDescriptor->hlslSource.find(gamutLine) != std::string::npos);
        TEST("GamutHlsli_BuiltinsMatchPastedForm", builtinCount > 0 && mismatchCount == 0);

        // The cache key covers the gamut text too.
        const std::string_view paramsText(GetShaderLabParamsHLSL(), GetShaderLabParamsHLSLLength());
        const std::string& colorMathText = GetColorMathHLSL();
        TEST("IncludeLibraryHash_ChangesWithGamut",
             IncludeLibraryHashOf(paramsText, colorMathText, gamutText) !=
             IncludeLibraryHashOf(paramsText, colorMathText, gamutText + "\n// edit\n"));
    }

    // Every registered built-in compiles, in each binding build.
    SL_TEST_CASE(TestBuiltinsCompile, Tag::Fast)
    {
        printf("\n=== Built-ins Compile ===\n");
        using namespace ShaderLab::Effects;
        struct BuiltinResult { std::wstring name; size_t builds{ 0 }; std::string failure; };
        std::vector<std::future<BuiltinResult>> pending;
        for (const auto& descriptor : ShaderLabEffects::Instance().All())
        {
            if (descriptor.IsUserEffect() || descriptor.hlslSource.empty()) continue;
            pending.push_back(std::async(std::launch::async, [&descriptor]
            {
                const auto node = ShaderLabEffects::CreateNode(descriptor);
                const auto& definition = *node.customEffect;
                const std::string target =
                    definition.shaderType == ShaderLab::Graph::CustomShaderType::PixelShader ? "ps_5_0" : "cs_5_0";
                BuiltinResult result{ descriptor.name };
                for (const auto& macroValues : BindingVariants(definition))
                {
                    std::vector<ShaderCompiler::MacroDef> macros;
                    for (const auto& [name, value] : macroValues) macros.push_back({ name.c_str(), value.c_str() });
                    auto compiled = ShaderCompiler::CompileFromString(
                        descriptor.hlslSource, "BuiltinCompileTest", "main", target, macros);
                    ++result.builds;
                    if (!compiled.succeeded && result.failure.empty())
                    {
                        result.failure = compiled.errors
                            ? std::string(static_cast<const char*>(compiled.errors->GetBufferPointer()),
                                          compiled.errors->GetBufferSize())
                            : std::string("no error text");
                    }
                }
                return result;
            }));
        }
        size_t builtinCount = 0, buildCount = 0, failureCount = 0;
        for (auto& future : pending)
        {
            const auto result = future.get();
            ++builtinCount;
            buildCount += result.builds;
            if (!result.failure.empty())
            {
                ++failureCount;
                printf("  [info] %ls does not compile: %.400s\n", result.name.c_str(), result.failure.c_str());
            }
        }
        printf("  [info] %zu built-ins, %zu builds\n", builtinCount, buildCount);
        TEST("Builtins_AllCompile", builtinCount >= 25 && failureCount == 0);
    }
}
