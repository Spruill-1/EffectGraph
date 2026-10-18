#include "pch_engine.h"
#include "../TestSupport.h"
#include "Effects/ColorMathCpu.h"
#include "Effects/Performance.h"
#include "Effects/ShaderVariants.h"
#include "Rendering/PixelReadback.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

// The ICtCp Tone Map's FP32 output against a CPU port of its HLSL, at every
// pixel of an HDR, partly wide-gamut pattern, with SourcePeakNits bound to a
// Luminance Statistics Max through the GPU binding and through the CPU
// readback. Statistics alone cannot catch a per-pixel fault, since they are
// the same for any permutation of the pixels.
namespace
{
    using namespace ShaderLab::Tests;
    namespace Cpu = ShaderLab::Effects::ColorMathCpu;

    constexpr int cSize = 64;

    // The pattern's colour at a pixel. Every value is exact in FP16, so the
    // D2D intermediate the compute input passes through loses nothing.
    Cpu::V3 PatternColor(int x, int y)
    {
        return { x * 0.25, y * 0.25, (x - y) * 0.0625 };
    }

    // m^-1, for the matrices ColorMathCpu holds only one way.
    std::array<double, 9> Invert(const double m[9])
    {
        const double det = m[0] * (m[4] * m[8] - m[5] * m[7])
                         - m[1] * (m[3] * m[8] - m[5] * m[6])
                         + m[2] * (m[3] * m[7] - m[4] * m[6]);
        return { (m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det, (m[1] * m[5] - m[2] * m[4]) / det,
                 (m[5] * m[6] - m[3] * m[8]) / det, (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
                 (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det, (m[0] * m[4] - m[1] * m[3]) / det };
    }

    double PqEotfSigned(double v)
    {
        const double nits = Cpu::PQ_EOTF(std::abs(v));
        return v < 0.0 ? -nits : nits;
    }

    // ICtCpToScRGB in the HLSL, including its clamp of PQ-encoded LMS.
    Cpu::V3 ICtCpToScRGB(Cpu::V3 ictcp)
    {
        static const auto ictcpToPqLms = Invert(Cpu::PQLMS_TO_ICTCP);
        static const auto lmsToXyz = Invert(Cpu::XYZ_TO_LMS_ICTCP);
        Cpu::V3 pq = Cpu::Mul(ictcpToPqLms.data(), ictcp);
        pq = { std::clamp(pq.x, -1.0, 1.0), std::clamp(pq.y, -1.0, 1.0), std::clamp(pq.z, -1.0, 1.0) };
        const Cpu::V3 lms{ PqEotfSigned(pq.x), PqEotfSigned(pq.y), PqEotfSigned(pq.z) };
        Cpu::V3 xyz = Cpu::Mul(lmsToXyz.data(), lms);
        xyz = { xyz.x / 80.0, xyz.y / 80.0, xyz.z / 80.0 };
        return Cpu::XYZToScRGB(xyz);
    }

    // The tone map at its defaults (Strength 1, ToneLift 0, KneeNits 0).
    Cpu::V3 ToneMapReference(Cpu::V3 rgb, double sourcePeakNits, double targetPeakNits)
    {
        Cpu::V3 ictcp = Cpu::ScRGBToICtCp(rgb);
        const double peakIn = Cpu::PQ_InvEOTF(sourcePeakNits);
        const double peakOut = Cpu::PQ_InvEOTF(targetPeakNits);
        const double kneeI = Cpu::PQ_InvEOTF(0.0);
        double compressed = ictcp.x;
        if (ictcp.x > kneeI && peakIn > peakOut)
        {
            const double i = ictcp.x - kneeI;
            const double in = peakIn - kneeI;
            const double out = peakOut - kneeI;
            const double pp = in * out;
            const double clamped = std::clamp(i, 0.0, in);
            compressed = kneeI + (pp <= 1e-12 ? 0.0 : clamped * pp / (std::max)(pp + clamped * (in - out), 1e-12));
        }
        ictcp.x = std::clamp(compressed / (std::max)(peakOut, 1e-6), 0.0, 1.0) * peakOut;
        return ICtCpToScRGB(ictcp);
    }

    struct ModeResult
    {
        float sourcePeakNits = -1.0f;
        ShaderLab::Effects::GpuBindingMode peakBinding = ShaderLab::Effects::GpuBindingMode::CBuffer;
        bool inputMatchesPattern = false;
        bool compressionEngaged = false;
        double maxRelativeError = INFINITY;
        double offPeakError = 0.0;      // against the reference at 1.25x the peak
    };

    ModeResult RunMode(bool gpuBindings)
    {
        ModeResult result;
        ScopedPerformanceSettings settings;
        ShaderLab::Performance::SetGpuBindingsEnabled(gpuBindings);

        auto& registry = ShaderLab::Effects::ShaderLabEffects::Instance();
        ShaderLab::Effects::SourceNodeFactory sourceFactory;
        ShaderLab::Graph::GraphModel graph;
        const auto pattern = PatternEffect(L"Tone Map Reference Pattern",
            "    return float4(pixel.x * 0.25, pixel.y * 0.25, (pixel.x - pixel.y) * 0.0625, 1.0);\n");
        const uint32_t patternId = AddPatternSource(graph, sourceFactory, pattern, cSize, cSize);
        const uint32_t statsId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"Luminance Statistics")));
        const uint32_t toneMapId = graph.AddNode(ShaderLab::Effects::ShaderLabEffects::CreateNode(
            *registry.FindByName(L"ICtCp Tone Map (HDR -> SDR)")));
        graph.Connect(patternId, 0, statsId, 0);
        graph.Connect(patternId, 0, toneMapId, 0);
        graph.BindProperty(toneMapId, L"SourcePeakNits", statsId, L"Max", 0);

        ShaderLab::Rendering::GraphEvaluator evaluator;
        evaluator.SetCpuAnalysisInterest({ statsId });
        for (int i = 0; i < 3; ++i)
        {
            graph.MarkAllDirty();
            ShaderLab::Rendering::FrameRequest request;
            ShaderLab::Rendering::FrameExecutor::Run(graph, evaluator, g_dc.get(), request);
        }
        if (const auto* toneMap = graph.FindNode(toneMapId); toneMap && !toneMap->runtimeError.empty())
            printf("    (debug) tone map error: %ls\n", toneMap->runtimeError.c_str());

        result.sourcePeakNits = AnalysisField(graph.FindNode(statsId), L"Max");
        size_t peakIndex = 0;
        for (const auto& param : graph.FindNode(toneMapId)->customEffect->parameters)
        {
            if (param.name == L"SourcePeakNits") break;
            if (param.gpuBindable) ++peakIndex;
        }
        result.peakBinding = ShaderLab::Effects::BindingModeAt(evaluator.AppliedGpuBits(toneMapId), peakIndex);
        const auto input = ShaderLab::Rendering::ReadPixelRegion(graph, patternId, 0, 0, cSize, cSize, g_dc.get());
        const auto output = ShaderLab::Rendering::ReadPixelRegion(graph, toneMapId, 0, 0, cSize, cSize, g_dc.get());
        constexpr size_t cFloats = static_cast<size_t>(cSize) * cSize * 4;
        if (input.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || input.pixels.size() != cFloats ||
            output.status != ShaderLab::Rendering::ReadPixelRegionStatus::Success || output.pixels.size() != cFloats ||
            result.sourcePeakNits <= 0.0f)
            return result;

        const double targetPeakNits = 203.0;
        result.inputMatchesPattern = true;
        result.maxRelativeError = 0.0;
        for (int y = 0; y < cSize; ++y)
        {
            for (int x = 0; x < cSize; ++x)
            {
                const size_t at = (static_cast<size_t>(y) * cSize + x) * 4;
                const Cpu::V3 rgb = PatternColor(x, y);
                if (input.pixels[at] != rgb.x || input.pixels[at + 1] != rgb.y || input.pixels[at + 2] != rgb.z)
                    result.inputMatchesPattern = false;
                const Cpu::V3 expected = ToneMapReference(rgb, result.sourcePeakNits, targetPeakNits);
                const double shown[3] = { output.pixels[at], output.pixels[at + 1], output.pixels[at + 2] };
                const double want[3] = { expected.x, expected.y, expected.z };
                const Cpu::V3 offPeak = ToneMapReference(rgb, result.sourcePeakNits * 1.25, targetPeakNits);
                const double wrong[3] = { offPeak.x, offPeak.y, offPeak.z };
                // Relative to the pixel's largest component: a saturated
                // colour's small components are differences of large terms.
                const double scale = (std::max)({ std::abs(want[0]), std::abs(want[1]), std::abs(want[2]), 1.0 / 64.0 });
                for (int c = 0; c < 3; ++c)
                {
                    result.maxRelativeError = (std::max)(result.maxRelativeError, std::abs(shown[c] - want[c]) / scale);
                    result.offPeakError = (std::max)(result.offPeakError, std::abs(shown[c] - wrong[c]) / scale);
                }
                if (x == cSize - 1 && y == cSize - 1)
                    result.compressionEngaged = expected.x < rgb.x * 0.5 && shown[0] < rgb.x * 0.5;
            }
        }
        return result;
    }

    SL_TEST_CASE(TestToneMapMatchesCpuReference, Tag::Gpu)
    {
        printf("\n=== ICtCp Tone Map vs CPU reference ===\n");
        // Shader float vs CPU double through PQ both ways. The HLSL's
        // LMS_TO_XYZ_ICTCP is the inverse of XYZ_TO_LMS_ICTCP to about 1e-4
        // (the reference inverts exactly), which shows as up to ~0.13% of a
        // pixel's largest component.
        constexpr double cTolerance = 1.0 / 256.0;
        for (const bool gpuBindings : { true, false })
        {
            const ModeResult mode = RunMode(gpuBindings);
            const char* label = gpuBindings ? "GpuBound" : "Cbuffer";
            printf("  [info] %s: SourcePeakNits %.2f, max relative error %.3g (%.3g against a 1.25x peak)\n",
                   label, mode.sourcePeakNits, mode.maxRelativeError, mode.offPeakError);
            const std::string prefix = std::string("ToneMapReference_") + label;
            TEST((prefix + "_PeakAboveTarget").c_str(), mode.sourcePeakNits > 203.0f);
            TEST((prefix + "_PeakTakesThatPath").c_str(), mode.peakBinding == (gpuBindings
                ? ShaderLab::Effects::GpuBindingMode::StructuredBuffer : ShaderLab::Effects::GpuBindingMode::CBuffer));
            TEST((prefix + "_InputIsPattern").c_str(), mode.inputMatchesPattern);
            TEST((prefix + "_CompressionEngaged").c_str(), mode.compressionEngaged);
            TEST((prefix + "_MatchesCpu").c_str(), mode.maxRelativeError < cTolerance);
            // The comparison can fail: a 25% peak error is far outside it.
            TEST((prefix + "_DetectsWrongPeak").c_str(), mode.offPeakError > 4.0 * cTolerance);
        }
    }
}
