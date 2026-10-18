#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestSupport.h"

#include <cstdio>
#include <cmath>

namespace ShaderLab::Tests
{
    // PQ_EOTF / PQ_InvEOTF round trips and a no-NaN guard for out-of-range
    // input.
    void TestTransferFunctions(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: Transfer Functions ===\n");

        // ---- API anchors ---------------------------------------------------
        // Note: PQ_InvEOTF in Effects/ColorMath.cpp takes L *in nits*
        // (it divides by 10000 internally), not normalized [0,1]. PQ_EOTF
        // takes the normalized PQ signal [0,1] and returns nits. So the
        // round trip "x nits -> PQ_InvEOTF -> N -> PQ_EOTF -> y nits" should
        // recover y == x within FP32 noise.

        // 100 nits → coding value ~0.5081. Standard BT.2100 / SMPTE ST.2084
        // inverse-EOTF reference value.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(PQ_InvEOTF(100.0), 0, 0, 0);
            )");
            TEST("PQ_InvEOTF(100 nits) ~= 0.5081 BT.2100 anchor",
                !r.empty() && Near(r[0].x, 0.5081f, 5e-3f));
        }
        // 1000 nits → coding value ~0.7515 (standard HDR reference).
        {
            auto r = bench.Run(R"(
                Result[0] = float4(PQ_InvEOTF(1000.0), 0, 0, 0);
            )");
            TEST("PQ_InvEOTF(1000 nits) ~= 0.7515 BT.2100 anchor",
                !r.empty() && Near(r[0].x, 0.7515f, 5e-3f));
        }
        // Peak: 10000 nits maps to coding value 1.0 by definition.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(PQ_InvEOTF(10000.0), 0, 0, 0);
            )");
            TEST("PQ_InvEOTF(10000 nits) == 1.0",
                !r.empty() && Near(r[0].x, 1.0f, 1e-5f));
        }
        // Zero: 0 nits maps to coding value 0.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(PQ_InvEOTF(0.0), 0, 0, 0);
            )");
            TEST("PQ_InvEOTF(0) == 0",
                !r.empty() && Near(r[0].x, 0.0f, 1e-6f));
        }
        // Forward direction sanity: PQ_EOTF(1.0) == 10000 nits.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(PQ_EOTF(1.0), 0, 0, 0);
            )");
            TEST("PQ_EOTF(1.0) == 10000 nits",
                !r.empty() && Near(r[0].x, 10000.0f, 1.0f));
        }
        // Forward direction zero.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(PQ_EOTF(0.0), 0, 0, 0);
            )");
            TEST("PQ_EOTF(0) == 0",
                !r.empty() && Near(r[0].x, 0.0f, 1e-3f));
        }

        // ---- Round trip ----------------------------------------------------
        // Sample 16 evenly-spaced nit values in [0, 10000]; round-trip
        // through PQ_InvEOTF (nits -> coding) and PQ_EOTF (coding -> nits)
        // and confirm the maximum absolute error is small.
        {
            auto r = bench.Run(R"(
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i <= 16; ++i)
                {
                    float nits = i * (10000.0 / 16.0);
                    float coding = PQ_InvEOTF(nits);
                    float rt = PQ_EOTF(coding);
                    maxErr = max(maxErr, abs(rt - nits));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("PQ round trip x -> InvEOTF -> EOTF -> x over [0,10000] nits (max err < 1.0)",
                !r.empty() && r[0].x < 1.0f);
        }

        // ---- Robustness ----------------------------------------------------
        // The codebase's PQ_EOTF has internal max(num, 0) + max(den, 1e-10)
        // guards, so out-of-range coding values produce a finite (if
        // out-of-range) nit value, not NaN/Inf. Confirm that invariant: a
        // FAIL here would mean someone removed a guard and unsuspecting
        // callers (HDR ColorMatrix, gamut boundary code) could now see NaN.
        {
            auto r = bench.Run(R"(
                float a = PQ_EOTF(1.2);     // past peak coding value
                float b = PQ_EOTF(-0.1);    // negative coding value
                float allFinite =
                    (isfinite(a) && isfinite(b)) ? 1.0 : 0.0;
                Result[0] = float4(allFinite, a, b, 0);
            )");
            TEST("PQ_EOTF is finite for out-of-range coding values [-0.1, 1.2]",
                !r.empty() && Near(r[0].x, 1.0f, 1e-6f));
        }

        // ICtCpToScRGB *must* be NaN-safe even at out-of-domain ICtCp
        // inputs (decision #52 footnote: pqLms = saturate(pqLms) before
        // PQ_EOTF). Construct an ICtCp that pushes the inverse matrix
        // above 1.0 in some LMS row and confirm scRGB is finite.
        {
            auto r = bench.Run(R"(
                // High-I, off-axis chroma that would normally drive an
                // LMS component out of [0,1] without the saturate guard.
                float3 ictcp = float3(0.9, 0.4, -0.3);
                float3 rgb = ICtCpToScRGB(ictcp);
                float finiteAll = (isfinite(rgb.r) && isfinite(rgb.g) && isfinite(rgb.b)) ? 1.0 : 0.0;
                Result[0] = float4(finiteAll, rgb.r, rgb.g, rgb.b);
            )");
            TEST("ICtCpToScRGB is finite on out-of-domain ICtCp (saturate guard)",
                !r.empty() && Near(r[0].x, 1.0f, 1e-6f));
        }
    }
}

namespace ShaderLab::Tests
{
    namespace
    {
        SL_REGISTER_TEST(TestTransferFunctions, Tag::Gpu | Tag::Math, [] { TestTransferFunctions(Bench()); });
    }
}
