#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestSupport.h"

#include <cstdio>

namespace ShaderLab::Tests
{
    // Tests for the anchored Möbius / Reinhard helpers used by the ICtCp
    // tone-mapping suite: the anchor, the peakIn/peakOut order at the call
    // site, and the asymptote outside the domain are each pinned here.
    void TestMobiusReinhard(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: Mobius / Reinhard ===\n");

        // ---- Anchor invariants --------------------------------------------
        // f(0) == 0
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ReinhardCompressI(0.0, 1.0, 0.5), 0, 0, 0);
            )");
            TEST("ReinhardCompressI(0, 1, 0.5) == 0",
                !r.empty() && Near(r[0].x, 0.0f, 1e-6f));
        }
        // f(peakIn) == peakOut (the 1.3.8 bug — was returning peakOut/(2-ratio))
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ReinhardCompressI(1.0, 1.0, 0.5), 0, 0, 0);
            )");
            TEST("ReinhardCompressI(peakIn, peakIn, peakOut) == peakOut [1.3.8 anchor regression]",
                !r.empty() && Near(r[0].x, 0.5f, 1e-5f));
        }
        // Same with non-trivial peak values.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ReinhardCompressI(0.7515, 0.7515, 0.5081), 0, 0, 0);
            )");
            TEST("ReinhardCompressI(1000nit_I, 1000nit_I, 100nit_I) == 100nit_I",
                !r.empty() && Near(r[0].x, 0.5081f, 1e-5f));
        }

        // ---- Slope at toe -------------------------------------------------
        // f'(0) ~= 1 (Reinhard feel near zero — small inputs pass through
        // approximately linearly). Numerical derivative at x=1e-4.
        {
            auto r = bench.Run(R"(
                float h = 1e-4;
                float f1 = ReinhardCompressI(h,   1.0, 0.5);
                float slope = f1 / h;
                Result[0] = float4(slope, 0, 0, 0);
            )");
            TEST("ReinhardCompressI: f'(0) ~= 1 (numerical slope at 1e-4 within 5%)",
                !r.empty() && Near(r[0].x, 1.0f, 5e-2f));
        }

        // ---- Round trip ---------------------------------------------------
        // Expand(Compress(I, hi, lo), hi, lo) == I — same peakIn/peakOut
        // convention on both sides. This is the invariant the 1.3.8
        // call-site inversion bug violated. Sample 16 points and report
        // worst-case.
        {
            auto r = bench.Run(R"(
                float maxErr = 0.0;
                float peakIn  = 0.7515;   // 1000-nit I
                float peakOut = 0.5081;   // 100-nit I
                [unroll]
                for (int i = 0; i <= 16; ++i) {
                    float I = (i / 16.0) * peakIn;
                    float c = ReinhardCompressI(I, peakIn, peakOut);
                    float rt = ReinhardExpandI(c, peakIn, peakOut);
                    maxErr = max(maxErr, abs(rt - I));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("Expand(Compress(I, hi, lo), hi, lo) == I round trip [1.3.8 inversion bug]",
                !r.empty() && r[0].x < 1e-4f);
        }

        // ---- Out-of-domain saturation -------------------------------------
        // Compress(2*peakIn, peakIn, peakOut) should saturate to peakOut,
        // not race to the asymptote (the 1.3.8 "1.4 million nits" bug).
        // The current impl clamps `Ic = clamp(I, 0, peakIn_I)`.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ReinhardCompressI(2.0, 1.0, 0.5), 0, 0, 0);
            )");
            TEST("ReinhardCompressI(2*peakIn, ...) saturates to peakOut [no asymptote walk]",
                !r.empty() && Near(r[0].x, 0.5f, 1e-5f));
        }
        // Negative input: clamp to 0.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ReinhardCompressI(-0.5, 1.0, 0.5), 0, 0, 0);
            )");
            TEST("ReinhardCompressI(negative, ...) clamps to 0",
                !r.empty() && Near(r[0].x, 0.0f, 1e-6f));
        }
        // Symmetric for Expand.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ReinhardExpandI(2.0 * 0.5, 1.0, 0.5), 0, 0, 0);
            )");
            TEST("ReinhardExpandI(2*peakOut, ...) saturates to peakIn",
                !r.empty() && Near(r[0].x, 1.0f, 1e-5f));
        }

        // ---- Monotonicity -------------------------------------------------
        // Sample 64 points in [0, peakIn]; verify each successive sample
        // is strictly greater than the previous.
        {
            auto r = bench.Run(R"(
                float prev = -1.0;
                float minDelta = 1e10;
                [unroll]
                for (int i = 0; i <= 63; ++i) {
                    float I = (i / 63.0) * 1.0;
                    float c = ReinhardCompressI(I, 1.0, 0.5);
                    if (i > 0) minDelta = min(minDelta, c - prev);
                    prev = c;
                }
                Result[0] = float4(minDelta, 0, 0, 0);
            )");
            TEST("ReinhardCompressI is monotonically increasing on [0, peakIn]",
                !r.empty() && r[0].x > 0.0f);
        }

        // ---- Degenerate peaks ---------------------------------------------
        // peakIn == 0 or peakOut == 0 should return 0 (the `pp <= 1e-12`
        // guard). Not testing what HLSL does with a NaN-producing path,
        // testing the explicit guard.
        {
            auto r = bench.Run(R"(
                float a = ReinhardCompressI(0.5, 0.0, 0.5);
                float b = ReinhardCompressI(0.5, 1.0, 0.0);
                Result[0] = float4(a, b, 0, 0);
            )");
            TEST("ReinhardCompressI returns 0 on degenerate peaks (zero guard)",
                !r.empty()
                && Near(r[0].x, 0.0f, 1e-6f)
                && Near(r[0].y, 0.0f, 1e-6f));
        }

        // ---- NitsToI / IToNits round trip ---------------------------------
        // These are thin wrappers but the I-space tone-mapping pipeline
        // composes them constantly; lock down that they cancel.
        {
            auto r = bench.Run(R"(
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i <= 16; ++i) {
                    float nits = i * (4000.0 / 16.0);
                    float rt = IToNits(NitsToI(nits));
                    maxErr = max(maxErr, abs(rt - nits));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("IToNits(NitsToI(x)) ~= x over [0, 4000] nits (max err < 1.0)",
                !r.empty() && r[0].x < 1.0f);
        }
    }
}

namespace ShaderLab::Tests
{
    namespace
    {
        SL_REGISTER_TEST(TestMobiusReinhard, Tag::Gpu | Tag::Math, [] { TestMobiusReinhard(Bench()); });
    }
}
