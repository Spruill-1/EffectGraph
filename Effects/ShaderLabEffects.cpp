#include "pch_engine.h"
#include "ShaderLabEffects.h"
#include "ColorMathCpu.h"
#include "../Graph/GraphModel.h"
#include "BytecodeCache.h"
#include "BuiltInEffects.h"
#include "EffectBlob.h"
#include "GamutLutGeometry.h"
#include "../Engine/Utf8.h"

namespace ShaderLab::Effects
{
    ShaderLabEffects& ShaderLabEffects::Instance()
    {
        static ShaderLabEffects instance;
        // Headers that registered at startup (EffectGraphAddEffects) are
        // parsed on first use, not under the loader lock they arrived in.
        DrainQueuedEffectBlobs();
        return instance;
    }


    // The guard lets a shader include the library more than once. It includes
    // the color math itself, so it can be the only include a shader names.
    const std::string& GetGamutHLSL()
    {
        static const std::string sGamutHLSL = []
        {
            std::string text = R"HLSL(
#ifndef SHADERLAB_GAMUT_HLSLI_INCLUDED
#define SHADERLAB_GAMUT_HLSLI_INCLUDED
// ---- ShaderLab Gamut Boundary Library ----
// The exact target-gamut boundary search in ICtCp, and the reader for the
// table the "ICtCp Gamut Boundary LUT" effect bakes from it. The table's
// producer and its readers compile this same text.
#include "shaderlab_colormath.hlsli"

// Boundary search against the target RGB cube. Bisection, not a closed form:
// the ray from neutral passes through PQ per channel, so the in-gamut set is
// not an analytic interval.
#define CUBE_EPS    1e-4
#define CUBE_COARSE 20
#define CUBE_REFINE 7

// Boundary table geometry, set from the engine's table size. W must be a
// power of two: the hue wrap below is a mask.
#define GAMUT_LUT_W @LUTW@
#define GAMUT_LUT_H @LUTH@
// The table's alpha is the target's fingerprint plus this offset. The offset
// depends on W, so a reader built for another width refuses the table.
#define GAMUT_LUT_STAMP_OFFSET (GAMUT_LUT_W * 0.5)

// Rows sit at I = peakOutI * (1 - (1 - s)^K), s = row / (H - 1), which
// concentrates them toward I(W), where the boundary collapses to zero.
// The generator and the reader both go through these two functions.
#define GAMUT_LUT_WARP_K 2.0
float GamutLutRowToI(float s, float peakOutI) { return peakOutI * (1.0 - pow(1.0 - saturate(s), GAMUT_LUT_WARP_K)); }
float GamutLutIToRow(float I, float peakOutI) { return 1.0 - pow(1.0 - saturate(I / max(peakOutI, 1e-6)), 1.0 / GAMUT_LUT_WARP_K); }

// True when this ICtCp coordinate reconstructs inside [0, peak] in the target
// RGB cube.
bool InTargetCube(float I, float2 ctcp, float peak, TargetXf t)
{
    // The tolerance absorbs ICtCp round-trip error, which scales with peak.
    float eps = max(1e-4, peak * 5e-4);
    float3 rgb = ScRGBToTarget(t, ICtCpToScRGB(float3(I, ctcp.x, ctcp.y)));
    return all(rgb >= -eps) && all(rgb <= peak + eps);
}

// The same test, also returning the reconstructed scRGB.
bool InTargetCubeRgb(float I, float2 ctcp, float peak, TargetXf t, out float3 scRGB)
{
    float eps = max(1e-4, peak * 5e-4);
    scRGB = ICtCpToScRGB(float3(I, ctcp.x, ctcp.y));
    float3 rgb = ScRGBToTarget(t, scRGB);
    return all(rgb >= -eps) && all(rgb <= peak + eps);
}

// Largest chroma radius along `dir` that stays inside the target cube.
// Coarse scan, then bisection. It keeps the LAST inside sample rather than
// the first exit: a constant-I ray can leave the cube and re-enter near its
// vertices, where saturated colours such as pure blue sit.
float CubeBoundaryRadius(float I, float2 dir, float peak, TargetXf t)
{
    if (!InTargetCube(I, float2(0.0, 0.0), peak, t))
        return 0.0;   // above the peak's own I even neutral is out of range

    // Wider than every preset's boundary. A Custom target wider than BT.2020
    // would be over-compressed here, never left out of gamut.
    const float kMaxR = 0.45;
    const float step  = kMaxR / CUBE_COARSE;

    float last = 0.0;
    [unroll]
    for (int i = 1; i <= CUBE_COARSE; ++i)
    {
        float r = step * i;
        if (InTargetCube(I, dir * r, peak, t)) last = r;
    }

    float lo = last;
    float hi = min(last + step, kMaxR);
    [unroll]
    for (int k = 0; k < CUBE_REFINE; ++k)
    {
        float mid = 0.5 * (lo + hi);
        if (InTargetCube(I, dir * mid, peak, t)) lo = mid;
        else                                  hi = mid;
    }
    return lo;
}

// B(I, hue) read back from a table built by CubeBoundaryRadius. Column x holds
// hue ((x + 0.5) / W) * 2pi - pi; row y holds I = GamutLutRowToI(y / (H - 1)).
// Integer Load, not Sample: the coordinates are computed, and D2D may place a
// lookup input inside a larger intermediate. The texture is a parameter so
// these helpers carry no register binding.

// The hue half of a table read: which two columns, and the blend between them.
// A search along one hue computes it once.
struct GamutLutColumn { int x0; int x1; float tx; };
GamutLutColumn GamutLutColumnFor(float2 dir)
{
    const float kTwoPi = 6.28318530718;
    float fx  = (atan2(dir.y, dir.x) / kTwoPi + 0.5) * GAMUT_LUT_W - 0.5;
    float x0f = floor(fx);
    GamutLutColumn column;
    column.tx = fx - x0f;
    column.x0 = ((int)x0f) & (GAMUT_LUT_W - 1);   // -1 wraps to W-1
    column.x1 = (column.x0 + 1) & (GAMUT_LUT_W - 1);
    return column;
}
float GamutLutRadiusAt(Texture2D<float4> lut, GamutLutColumn c, float I, float peakOutI)
{
    float fy  = GamutLutIToRow(I, peakOutI) * (GAMUT_LUT_H - 1);
    int   y0  = min((int)fy, GAMUT_LUT_H - 2);
    float ty  = fy - y0;
    float b00 = lut.Load(int3(c.x0, y0,     0)).r;
    float b10 = lut.Load(int3(c.x1, y0,     0)).r;
    float b01 = lut.Load(int3(c.x0, y0 + 1, 0)).r;
    float b11 = lut.Load(int3(c.x1, y0 + 1, 0)).r;
    return lerp(lerp(b00, b10, c.tx), lerp(b01, b11, c.tx), ty);
}
float GamutLutBoundaryRadius(Texture2D<float4> lut, float I, float2 dir, float peakOutI)
{
    return GamutLutRadiusAt(lut, GamutLutColumnFor(dir), I, peakOutI);
}

// Table stamp, in every texel: .g = the peak it was built for, .b = the I its
// rows span, .a = GamutLutStampAlpha(target). An unwired lookup input reads 0.
float GamutLutStampAlpha(TargetXf t) { return t.fingerprint + GAMUT_LUT_STAMP_OFFSET; }

// True when a table's stamp matches this target, peak and table width. A
// reader that gets false runs CubeBoundaryRadius instead.
bool GamutLutStampMatches(float4 stamp, TargetXf t, float peak)
{
    return (stamp.a > 0.5) &&
           (abs(stamp.a - GAMUT_LUT_STAMP_OFFSET - t.fingerprint) <= 1e-5 * t.fingerprint) &&
           (abs(stamp.g - peak) <= 1e-4 * max(peak, 1.0));
}

#endif
)HLSL";
            auto put = [&text](const char* key, uint32_t value)
            {
                text.replace(text.find(key), std::strlen(key), std::to_string(value));
            };
            put("@LUTW@", cGamutLutWidth);
            put("@LUTH@", cGamutLutHeight);
            return text;
        }();
        return sGamutHLSL;
    }

    ShaderLabEffects::ShaderLabEffects()
    {
        RegisterAlwaysAvailable();
    }

    const ShaderLabEffectDescriptor* ShaderLabEffects::FindByName(std::wstring_view name) const
    {
        for (const auto& e : m_effects)
            if (::CompareStringOrdinal(e.name.data(), static_cast<int>(e.name.size()),
                    name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL)
                return &e;
        return nullptr;
    }

    namespace
    {
        // Renamed effects: { old id, current id }. Saved graphs that carry an
        // old id still resolve, so they load and offer "Update Effect".
        constexpr std::pair<std::wstring_view, std::wstring_view> scLegacyEffectIds[] = {
            { L"Perceptual Gamut Map", L"ICtCp Gamut Map" },
        };
    }

    const ShaderLabEffectDescriptor* ShaderLabEffects::FindById(std::wstring_view effectId) const
    {
        for (const auto& e : m_effects)
            if (e.effectId == effectId)
                return &e;
        for (const auto& [legacyId, currentId] : scLegacyEffectIds)
            if (effectId == legacyId)
                return FindById(currentId);
        return nullptr;
    }

    namespace
    {
        class CatalogEffectHooks final : public IEffectHooks
        {
        public:
            const ShaderLabEffectDescriptor::DerivedConstantsFn*
                DerivedConstants(std::wstring_view effectId) const override
            {
                const auto* descriptor = ShaderLabEffects::Instance().FindById(effectId);
                return (descriptor && descriptor->deriveConstants) ? &descriptor->deriveConstants : nullptr;
            }

            const ShaderLabEffectDescriptor::ImageOutputSizeFn*
                ImageOutputSize(std::wstring_view effectId) const override
            {
                const auto* descriptor = ShaderLabEffects::Instance().FindById(effectId);
                return (descriptor && descriptor->deriveImageOutputSize) ? &descriptor->deriveImageOutputSize : nullptr;
            }
        };
    }

    const IEffectHooks& BuiltInEffectHooks()
    {
        static const CatalogEffectHooks sHooks;
        return sHooks;
    }

    std::vector<const ShaderLabEffectDescriptor*> ShaderLabEffects::ByCategory(std::wstring_view category) const
    {
        std::vector<const ShaderLabEffectDescriptor*> result;
        for (const auto& e : m_effects)
            if (e.category == category)
                result.push_back(&e);
        return result;
    }

    std::vector<std::wstring> ShaderLabEffects::Categories() const
    {
        std::vector<std::wstring> cats;
        for (const auto& e : m_effects)
        {
            bool found = false;
            for (const auto& c : cats) if (c == e.category) { found = true; break; }
            if (!found) cats.push_back(e.category);
        }
        return cats;
    }

    Graph::EffectNode ShaderLabEffects::CreateNode(const ShaderLabEffectDescriptor& desc)
    {
        using namespace Graph;

        EffectNode node;
        node.name = desc.name;
        node.type = (desc.shaderType == CustomShaderType::PixelShader)
            ? NodeType::PixelShader : NodeType::ComputeShader;

        // Parameter nodes (no HLSL) and data-only effects have no image output pin.
        if (!desc.hlslSource.empty() && !desc.dataOnly)
            node.outputPins.push_back({ L"Output", 0 });

        CustomEffectDefinition def;
        def.shaderType = desc.shaderType;
        def.hlslSource = Utf8ToWide(desc.hlslSource);
        def.inputNames = desc.inputNames;
        def.lookupInputCount = desc.lookupInputCount;
        def.variadicInputs = desc.variadicInputs;
        def.parameters = desc.parameters;
        def.analysisFields = desc.analysisFields;
        def.analysisOutputType = desc.analysisOutputType;
        def.threadGroupX = desc.threadGroupX;
        def.threadGroupY = desc.threadGroupY;
        def.threadGroupZ = desc.threadGroupZ;
        def.shaderLabEffectId = desc.effectId;
        def.shaderLabEffectVersion = desc.effectVersion;
        def.nodeKind = desc.nodeKind;
        def.dispatchShape = desc.dispatchShape;
        def.outputExtent = desc.outputExtent;
        def.injectedConstants = desc.injectedConstants;
        CoCreateGuid(&def.shaderGuid);

        // Set up input pins from input names. A variadic effect starts with
        // two; GraphModel::FitVariadicPins adds more as inputs connect.
        const size_t initialPinCount = desc.variadicInputs
            ? (std::min<size_t>)(2, desc.inputNames.size())
            : desc.inputNames.size();
        for (uint32_t i = 0; i < initialPinCount; ++i)
            node.inputPins.push_back({ std::format(L"I{}", i), i });

        // Set default property values from parameter definitions.
        for (const auto& param : desc.parameters)
            node.properties[param.name] = param.defaultValue;

        // Set hidden default properties (cbuffer values not in Properties panel).
        for (const auto& [key, val] : desc.hiddenDefaults)
            node.properties[key] = val;

        node.customEffect = std::move(def);
        return node;
    }

    // The nodes every host has, shader or not: parameters, clocks, expressions
    // and the working space. Built-in shader effects come from the headers
    // EffectGraphCompile generates (Effects/BuiltInEffects.h).
    void ShaderLabEffects::RegisterAlwaysAvailable()
    {
        // ---- Parameter: Float Slider ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Float Parameter";
            desc.effectId = L"Float Parameter"; desc.effectVersion = 1;
            desc.nodeKind = Graph::NodeKind::ConstantParameter;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            // No HLSL — evaluator handles parameter nodes directly.
            desc.parameters = {
                { L"Value", L"float", 0.5f, 0.0f, 1.0f, 0.01f },
                { L"Min",   L"float", 0.0f, -10000.0f, 10000.0f, 0.1f },
                { L"Max",   L"float", 1.0f, -10000.0f, 10000.0f, 0.1f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Integer Slider ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Integer Parameter";
            desc.effectId = L"Integer Parameter"; desc.effectVersion = 1;
            desc.nodeKind = Graph::NodeKind::ConstantParameter;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 0.0f, 0.0f, 10.0f, 1.0f },
                { L"Min",   L"float", 0.0f, -10000.0f, 10000.0f, 1.0f },
                { L"Max",   L"float", 10.0f, -10000.0f, 10000.0f, 1.0f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Toggle ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Toggle Parameter";
            desc.effectId = L"Toggle Parameter"; desc.effectVersion = 1;
            desc.nodeKind = Graph::NodeKind::ConstantParameter;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Value", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Gamut Selector ----
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Gamut Parameter";
            desc.effectId = L"Gamut Parameter"; desc.effectVersion = 3;
            desc.nodeKind = Graph::NodeKind::ConstantParameter;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                GamutParameter(L"Value", 0.0f),
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Value", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Parameter: Clock ----
        // Time-based animation source. Outputs elapsed Time (seconds) and
        // Progress (0→1 normalized). Has on-node Play/Pause and seek slider.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Clock";
            desc.effectId = Graph::cClockEffectId; desc.effectVersion = 2;
            desc.nodeKind = Graph::NodeKind::Clock;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"AutoDuration", L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
                { L"StartTime", L"float", 0.0f, 0.0f, 3600.0f, 0.1f },
                { L"StopTime",  L"float", 10.0f, 0.0f, 3600.0f, 0.1f },
                { L"Speed",     L"float", 1.0f, -10.0f, 10.0f, 0.1f },
                { L"Loop",      L"float", 1.0f, 0.0f, 1.0f, 1.0f, { L"Off", L"On" } },
                // How often the clock actually EMITS, in Hz. 0 = every frame,
                // which is what it always did. Above 0, Time and Progress are
                // quantised to 1/rate and consumers are dirtied only when that
                // quantised value changes -- so the rate controls how often
                // downstream work is invalidated, not merely what number is
                // reported. That distinction is the point: a clock that
                // dirties every frame makes every downstream node re-run every
                // frame regardless of how slowly its value is moving.
                { L"UpdateRate", L"float", 0.0f, 0.0f, 240.0f, 1.0f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Time",     Graph::AnalysisFieldType::Float },
                { L"Progress", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Numeric Expression Parameter Node ----
        // A single configurable math node that evaluates a user-provided
        // expression with up to 5 named scalar inputs. Replaces the older
        // Add/Subtract/Multiply/Divide/Min/Max nodes — all of those are
        // expressible as a one-line formula here (e.g. "max(A,B)", "A*B+C").
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Numeric Expression";
            desc.effectId = Graph::cMathExpressionEffectId; desc.effectVersion = 1;
            desc.nodeKind = Graph::NodeKind::Expression;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Expression", L"string", std::wstring(L"A"), 0.0f, 0.0f, 0.0f },
                { L"A", L"float", 0.0f, -100000.0f, 100000.0f, 0.1f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Result", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Random Parameter Node ----
        // Takes a single float Seed and produces a deterministic, well-mixed
        // float in [0, 1]. The output is a pure function of the seed value:
        // whenever the upstream value changes (e.g. driven by a Clock or
        // Numeric Expression), a fresh random number is produced; identical
        // seeds always reproduce the same output, which keeps graphs and
        // pixel-trace results deterministic.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Random";
            desc.effectId = Graph::cRandomEffectId; desc.effectVersion = 1;
            desc.nodeKind = Graph::NodeKind::Random;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            desc.parameters = {
                { L"Seed", L"float", 0.0f, -1000000.0f, 1000000.0f, 0.01f },
            };
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                { L"Result", Graph::AnalysisFieldType::Float },
            };
            m_effects.push_back(std::move(desc));
        }

        // ---- Working Space Parameter Node ----
        // A first-class, host-driven parameter node that mirrors the active
        // display profile (live or simulated) as 14 typed analysis output
        // fields. Lets graphs wire other nodes (tone mapper, OOG, etc.) to
        // the live working space via the property-binding system instead of
        // duplicating per-effect "follow the display" toggles. Hosts push the
        // display profile into node->properties, keyed by field name, through
        // Rendering::UpdateWorkingSpaceNodes whenever it changes; the
        // evaluator's parameter-node path unpacks them into the analysis
        // output.
        {
            ShaderLabEffectDescriptor desc;
            desc.name = L"Working Space";
            desc.effectId = Graph::cWorkingSpaceEffectId; desc.effectVersion = 1;
            desc.nodeKind = Graph::NodeKind::WorkingSpace;
            desc.category = L"Parameter";
            desc.shaderType = Graph::CustomShaderType::PixelShader;
            // No parameters — values are entirely host-driven. Properties
            // panel will be empty for this node, exactly the desired UX.
            desc.analysisOutputType = Graph::AnalysisOutputType::Typed;
            desc.analysisFields = {
                // Display mode (Float, encoded as 0=SDR, 1=WCG/ACM, 2=HDR).
                { L"ActiveColorMode",   Graph::AnalysisFieldType::Float },
                // Capability + user-toggle flags (Float-as-bool, 0/1).
                { L"HdrSupported",      Graph::AnalysisFieldType::Float },
                { L"HdrUserEnabled",    Graph::AnalysisFieldType::Float },
                { L"WcgSupported",      Graph::AnalysisFieldType::Float },
                { L"WcgUserEnabled",    Graph::AnalysisFieldType::Float },
                { L"IsSimulated",       Graph::AnalysisFieldType::Float },
                // Luminance levels in nits.
                { L"SdrWhiteNits",      Graph::AnalysisFieldType::Float },
                { L"PeakNits",          Graph::AnalysisFieldType::Float },
                { L"MinNits",           Graph::AnalysisFieldType::Float },
                { L"MaxFullFrameNits",  Graph::AnalysisFieldType::Float },
                // Color primaries as CIE xy (Float2).
                { L"RedPrimary",        Graph::AnalysisFieldType::Float2 },
                { L"GreenPrimary",      Graph::AnalysisFieldType::Float2 },
                { L"BluePrimary",       Graph::AnalysisFieldType::Float2 },
                { L"WhitePoint",        Graph::AnalysisFieldType::Float2 },
            };
            // Bootstrap defaults — sRGB SDR, shown until the host's first
            // UpdateWorkingSpaceNodes.
            using winrt::Windows::Foundation::Numerics::float2;
            desc.hiddenDefaults = {
                { L"ActiveColorMode",   Graph::PropertyValue{ 0.0f } },
                { L"HdrSupported",      Graph::PropertyValue{ 0.0f } },
                { L"HdrUserEnabled",    Graph::PropertyValue{ 0.0f } },
                { L"WcgSupported",      Graph::PropertyValue{ 0.0f } },
                { L"WcgUserEnabled",    Graph::PropertyValue{ 0.0f } },
                { L"IsSimulated",       Graph::PropertyValue{ 0.0f } },
                { L"SdrWhiteNits",      Graph::PropertyValue{ 80.0f } },
                { L"PeakNits",          Graph::PropertyValue{ 80.0f } },
                { L"MinNits",           Graph::PropertyValue{ 0.5f } },
                { L"MaxFullFrameNits",  Graph::PropertyValue{ 80.0f } },
                { L"RedPrimary",        Graph::PropertyValue{ float2{ 0.640f, 0.330f } } },
                { L"GreenPrimary",      Graph::PropertyValue{ float2{ 0.300f, 0.600f } } },
                { L"BluePrimary",       Graph::PropertyValue{ float2{ 0.150f, 0.060f } } },
                { L"WhitePoint",        Graph::PropertyValue{ float2{ 0.3127f, 0.3290f } } },
            };
            m_effects.push_back(std::move(desc));
        }
    }
}
