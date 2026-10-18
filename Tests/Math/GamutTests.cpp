#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestSupport.h"
#include "Effects/ColorMathCpu.h"
#include "Effects/ShaderLabEffects.h"

#include <cstdio>

namespace ShaderLab::Tests
{
    void TestSrgbTransfer(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: sRGB transfer ===\n");

        {
            // Linear 1.0 -> sRGB 1.0 anchor.
            auto r = bench.Run(R"(
                Result[0] = float4(LinearToSRGB(float3(1.0, 0.5, 0.0)), 0);
            )");
            TEST("LinearToSRGB(1,0.5,0) ~= (1.0, 0.7354, 0.0) anchor",
                !r.empty()
                && Near(r[0].x, 1.0000f, 1e-4f)
                && Near(r[0].y, 0.7354f, 1e-3f)
                && Near(r[0].z, 0.0000f, 1e-4f));
        }
        {
            // sRGB 0.5 -> linear ~= 0.2140. Standard sRGB anchor.
            auto r = bench.Run(R"(
                Result[0] = float4(SRGBToLinear(float3(0.5, 0.5, 0.5)), 0);
            )");
            TEST("SRGBToLinear(0.5,0.5,0.5) ~= (0.2140) BT.709 anchor",
                !r.empty()
                && Near(r[0].x, 0.2140f, 1e-3f)
                && Near(r[0].y, 0.2140f, 1e-3f)
                && Near(r[0].z, 0.2140f, 1e-3f));
        }
        {
            // Toe behavior: tiny values use the linear segment so should
            // round-trip exactly. SRGBToLinear(0.04045) is exactly the
            // linear-segment cutoff.
            auto r = bench.Run(R"(
                float a = SRGBToLinear(float3(0.04045, 0, 0)).x;
                float b = SRGBToLinear(float3(0.0, 0, 0)).x;
                Result[0] = float4(a, b, 0, 0);
            )");
            TEST("SRGBToLinear toe: 0.04045 ~= 0.04045/12.92, 0 == 0",
                !r.empty()
                && Near(r[0].x, 0.04045f / 12.92f, 1e-6f)
                && Near(r[0].y, 0.0f, 1e-7f));
        }
        {
            // sRGB <-> linear 6-color round trip.
            auto r = bench.Run(R"(
                float3 colors[6] = {
                    float3(0.0, 0.0, 0.0),
                    float3(0.5, 0.5, 0.5),
                    float3(1.0, 1.0, 1.0),
                    float3(1.0, 0.0, 0.0),
                    float3(0.0, 1.0, 0.0),
                    float3(0.18, 0.5, 0.9)
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 6; ++i) {
                    float3 rt = LinearToSRGB(SRGBToLinear(colors[i]));
                    float3 d = abs(rt - colors[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("LinearToSRGB(SRGBToLinear(x)) ~= x on 6-color suite",
                !r.empty() && r[0].x < 1e-5f);
        }
    }

    void TestXyYConversions(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: xyY and XYZ ===\n");

        {
            // xyYToXYZ at D65 anchor: x=0.3127, y=0.3290, Y=1.0 -> standard
            // D65 XYZ (0.95047, 1.0, 1.08883). The codebase's D65 white
            // constant uses (0.3127, 0.3290) so test against the expected
            // X = x*Y/y = 0.95046, Z = (1-x-y)*Y/y = 1.08906 (using these
            // exact xy values; the canonical D65 XYZ uses slightly
            // different rounding so the test compares to the algebraic
            // result, not the ScRGBToXYZ constant).
            auto r = bench.Run(R"(
                Result[0] = float4(xyYToXYZ(float3(0.3127, 0.3290, 1.0)), 0);
            )");
            // Expected: X = 0.3127/0.3290 = 0.9504559; Y = 1.0; Z = (1-0.3127-0.3290)/0.3290 = 1.0890578.
            TEST("xyYToXYZ(D65 chromaticities, Y=1.0) ~= algebraic XYZ",
                !r.empty()
                && Near(r[0].x, 0.9504559f, 1e-4f)
                && Near(r[0].y, 1.0000000f, 1e-6f)
                && Near(r[0].z, 1.0890578f, 1e-4f));
        }
        {
            // Round trip on D65 + the three Rec.709 primaries (need Y > 0
            // since Y == 0 collapses xy via the y < 1e-10 branch).
            auto r = bench.Run(R"(
                float3 xyY[4] = {
                    float3(0.3127, 0.3290, 1.0),  // D65
                    float3(0.6400, 0.3300, 0.5),  // Rec.709 R
                    float3(0.3000, 0.6000, 0.5),  // Rec.709 G
                    float3(0.1500, 0.0600, 0.5)   // Rec.709 B
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 4; ++i) {
                    float3 rt = XYZToxyY(xyYToXYZ(xyY[i]));
                    float3 d = abs(rt - xyY[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("xyYToXYZ <-> XYZToxyY round trip on D65 + Rec.709 primaries",
                !r.empty() && r[0].x < 1e-5f);
        }
        {
            // Y == 0 degenerate branch returns D65 chromaticity by
            // convention (the codebase guards y<1e-10 returning (0.3127, 0.3290)).
            // Pass tiny non-zero values below the 1e-10 threshold so the
            // HLSL compiler can't statically prove a divide-by-zero in
            // the not-taken branch (X4008 → X3129 warnings-as-errors).
            auto r = bench.Run(R"(
                Result[0] = float4(XYZToxyY(float3(1e-15, 1e-15, 1e-15)), 0);
            )");
            TEST("XYZToxyY(near-zero) returns D65 chromaticity fallback",
                !r.empty()
                && Near(r[0].x, 0.3127f, 1e-4f)
                && Near(r[0].y, 0.3290f, 1e-4f));
        }
    }

    void TestPointInTriangle(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: PointInTriangle ===\n");

        // D65 (0.3127, 0.3290) is inside the Rec.709 triangle.
        {
            auto r = bench.Run(R"(
                bool inside = PointInTriangle(D65_WHITE, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B);
                Result[0] = float4(inside ? 1.0 : 0.0, 0, 0, 0);
            )");
            TEST("PointInTriangle: D65 is inside Rec.709",
                !r.empty() && Near(r[0].x, 1.0f, 1e-6f));
        }
        // BT.2020 red primary (0.708, 0.292) is OUTSIDE Rec.709
        // (Rec.709 R is 0.640, 0.330 — much less saturated).
        {
            auto r = bench.Run(R"(
                bool inside = PointInTriangle(GAMUT_2020_R, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B);
                Result[0] = float4(inside ? 1.0 : 0.0, 0, 0, 0);
            )");
            TEST("PointInTriangle: BT.2020 red primary is OUTSIDE Rec.709",
                !r.empty() && Near(r[0].x, 0.0f, 1e-6f));
        }
        // Rec.709 R is itself inside (boundary point — barycentric u==1, v==0).
        {
            auto r = bench.Run(R"(
                bool inside = PointInTriangle(GAMUT_709_R, GAMUT_709_R, GAMUT_709_G, GAMUT_709_B);
                Result[0] = float4(inside ? 1.0 : 0.0, 0, 0, 0);
            )");
            TEST("PointInTriangle: Rec.709 R primary is on/inside Rec.709 boundary",
                !r.empty() && Near(r[0].x, 1.0f, 1e-6f));
        }
    }

    void TestOklab(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: OKLab ===\n");

        // OKLab(white) should land on (1, 0, 0): lightness 1, neutral chroma.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(LinearToOKLab(float3(1, 1, 1)), 0);
            )");
            TEST("LinearToOKLab(white) ~= (1, 0, 0)",
                !r.empty()
                && Near(r[0].x, 1.0f, 1e-3f)
                && Near(r[0].y, 0.0f, 1e-3f)
                && Near(r[0].z, 0.0f, 1e-3f));
        }
        // OKLab(black) should land on (0, 0, 0).
        {
            auto r = bench.Run(R"(
                Result[0] = float4(LinearToOKLab(float3(0, 0, 0)), 0);
            )");
            TEST("LinearToOKLab(black) ~= (0, 0, 0)",
                !r.empty()
                && Near(r[0].x, 0.0f, 1e-5f)
                && Near(r[0].y, 0.0f, 1e-5f)
                && Near(r[0].z, 0.0f, 1e-5f));
        }
    }

    // CIE Lab(scRGB white) should be approximately (100, 0, 0)
    // assuming Y_n=1.0 and the scRGB pipeline treats 1.0 as the white
    // point. (Tolerance loose because the codebase's D65_XYZ
    // constants don't match the matrix's white point exactly.)
    void TestScRgbToLabAnchor(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: ScRGBToLab anchor ===\n");

        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToLab(float3(1, 1, 1)), 0);
            )");
            TEST("ScRGBToLab(white) ~= (100, 0, 0) within Lab D65 rounding",
                !r.empty()
                && Near(r[0].x, 100.0f, 0.5f)
                && Near(r[0].y, 0.0f,   1.0f)
                && Near(r[0].z, 0.0f,   1.0f));
        }
    }

    // Contract tests: these hold for any hardness p >= 1 of the ACES
    // power curve (p=1 is exactly Reinhard). threshold=0.75,
    // limit=1.5, power=1.2 (the ACES RGC default) unless stated.
    void TestSoftCompressDistance(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: SoftCompressDistance ===\n");

        {
            // Identity zone: d <= threshold returns d exactly.
            auto r = bench.Run(R"(
                float a = SoftCompressDistance(0.30, 0.75, 1.5, 1.2);
                float b = SoftCompressDistance(0.75, 0.75, 1.5, 1.2);
                Result[0] = float4(a, b, 0, 0);
            )");
            TEST("SoftCompressDistance: identity below threshold",
                !r.empty()
                && Near(r[0].x, 0.30f, 1e-6f)
                && Near(r[0].y, 0.75f, 1e-5f));
        }
        {
            // Anchor: d == limit lands exactly on the boundary (1.0).
            auto r = bench.Run(R"(
                Result[0] = float4(SoftCompressDistance(1.5, 0.75, 1.5, 1.2), 0, 0, 0);
            )");
            TEST("SoftCompressDistance: limit maps onto boundary (== 1.0)",
                !r.empty() && Near(r[0].x, 1.0f, 1e-3f));
        }
        {
            // C1 join: slope ~= 1 just above threshold, so gradients
            // crossing the knee don't kink. Central-difference slope over
            // [t, t + 0.02] must be within 15% of 1.
            auto r = bench.Run(R"(
                float y0 = SoftCompressDistance(0.75, 0.75, 1.5, 1.2);
                float y1 = SoftCompressDistance(0.77, 0.75, 1.5, 1.2);
                Result[0] = float4((y1 - y0) / 0.02, 0, 0, 0);
            )");
            TEST("SoftCompressDistance: slope ~= 1 entering the knee (C1)",
                !r.empty() && Near(r[0].x, 1.0f, 0.15f));
        }
        {
            // Monotone + compressive: outputs strictly increase with d,
            // and never exceed the input above the threshold.
            auto r = bench.Run(R"(
                float d[5] = { 0.8, 1.0, 1.2, 1.4, 1.5 };
                float prev = -1.0;
                float mono = 1.0, comp = 1.0;
                [unroll]
                for (int i = 0; i < 5; ++i) {
                    float y = SoftCompressDistance(d[i], 0.75, 1.5, 1.2);
                    if (y <= prev)  mono = 0.0;
                    if (y > d[i] + 1e-5) comp = 0.0;
                    prev = y;
                }
                Result[0] = float4(mono, comp, 0, 0);
            )");
            TEST("SoftCompressDistance: monotone increasing and compressive",
                !r.empty()
                && Near(r[0].x, 1.0f, 1e-6f)
                && Near(r[0].y, 1.0f, 1e-6f));
        }
        {
            // Softness proper: a point between threshold and limit must map
            // strictly BELOW the hard boundary (that's the whole feature —
            // headroom is reserved so d in (1, limit] stays ordered instead
            // of flattening onto the shell).
            auto r = bench.Run(R"(
                Result[0] = float4(SoftCompressDistance(1.2, 0.75, 1.5, 1.2), 0, 0, 0);
            )");
            TEST("SoftCompressDistance: interior of knee stays below boundary (soft, not clip)",
                !r.empty() && r[0].x < 0.999f && r[0].x > 0.75f);
        }
        {
            // Hardness ordering: higher power tracks identity longer, so at
            // a fixed d inside the knee it must compress LESS than p=1
            // (Reinhard). Also pins p=1 == Reinhard closed form:
            // s = (1-t)(l-t)/(l-1) = 0.375; y(1.0) = t + s*x/(s+x) = 0.90.
            auto r = bench.Run(R"(
                float soft = SoftCompressDistance(1.0, 0.75, 1.5, 1.0);
                float hard = SoftCompressDistance(1.0, 0.75, 1.5, 3.0);
                Result[0] = float4(soft, hard, 0, 0);
            )");
            TEST("SoftCompressDistance: p=1 matches Reinhard; higher hardness compresses less",
                !r.empty()
                && Near(r[0].x, 0.90f, 1e-3f)
                && r[0].y > r[0].x + 0.01f);
        }
    }

    void TestDitherQuantize(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: 8-bit dither and quantize ===\n");

        {
            // Zero strength must land exactly on the 8-bit grid.
            auto r = bench.Run(R"(
                float3 q = DitherQuantize(float3(0.5, 0.25, 0.75), float2(3, 7), 256.0, 0.0);
                float3 grid = round(float3(0.5, 0.25, 0.75) * 255.0) / 255.0;
                Result[0] = float4(abs(q - grid), 0);
            )");
            TEST("DitherQuantize(strength 0) lands exactly on the 8-bit grid",
                !r.empty() && r[0].x < 1e-6f && r[0].y < 1e-6f && r[0].z < 1e-6f);
        }
        {
            // The dither stays inside its nominal +-1 LSB across a pixel sweep.
            auto r = bench.Run(R"(
                float lo = 1e9, hi = -1e9;
                [unroll]
                for (int i = 0; i < 32; ++i) {
                    float d = RotatedIgnDither(float2(i, i * 3 + 1));
                    lo = min(lo, d); hi = max(hi, d);
                }
                Result[0] = float4(lo, hi, 0, 0);
            )");
            TEST("RotatedIgnDither stays within its nominal [-1, 1]",
                !r.empty() && r[0].x >= -1.0f && r[0].y <= 1.0f && r[0].y > r[0].x);
        }
        {
            // Pin the ACTUAL distribution, which is neither triangular nor
            // trapezoidal. IGN(p) and IGN(p + 5.588238) are not independent:
            // the offset shifts dot(p,k) by 0.407649, which the x52.98
            // amplification turns into a fixed rotation of n1 -- +0.59844 or
            // +0.61552 depending on whether the inner frac wraps (62%/38%,
            // never neither). n1 + frac(n1 + r) - 1 is then a 2-slope sawtooth
            // whose slopes each map uniform n1 to a UNIFORM output, on +-(1-r)
            // and +-r at density 0.5. Superposing them gives a STEPPED density
            // -- 1.0 on the plateau, 0.5 on the shoulders, vertical risers, no
            // ramps. (A trapezoid needs a convolution, not a superposition.)
            // Consequences: symmetric and mean-preserving (good, see below), but
            // variance 0.0945 rather than a real TPDF's 1/6, and support +-0.615
            // rather than +-1. If someone decorrelates the second sample this
            // test fails -- that is intentional. It is a real behaviour change
            // (output noise ~0.31 -> ~0.41 codes RMS), not a cleanup.
            auto r = bench.Run(R"(
                float s1 = 0, s2 = 0, mx = 0;
                for (int y = 0; y < 64; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        float d = RotatedIgnDither(float2(x, y));
                        s1 += d; s2 += d * d; mx = max(mx, abs(d));
                    }
                }
                Result[0] = float4(s1 / 4096.0, s2 / 4096.0, mx, 0);
            )");
            TEST("RotatedIgnDither is a symmetric stepped density, not a true TPDF",
                !r.empty()
                && fabs(r[0].x) < 0.005f            // symmetric: mean ~ 0
                && Near(r[0].y, 0.0946f, 0.004f)    // variance, NOT 1/6 = 0.1667
                && Near(r[0].z, 0.6152f, 0.01f));   // support, NOT 1.0
        }
        {
            // A value sitting between two codes must resolve to BOTH codes
            // across pixels -- that is the whole mechanism by which dither
            // trades banding for noise. 0.5 encodes to ~187.5/255.
            auto r = bench.Run(R"(
                float v = 187.5 / 255.0;
                float lo = 1e9, hi = -1e9;
                [unroll]
                for (int i = 0; i < 32; ++i) {
                    float q = DitherQuantize(float3(v, v, v), float2(i, i * 5 + 2), 256.0, 1.0).x;
                    lo = min(lo, q); hi = max(hi, q);
                }
                Result[0] = float4(lo * 255.0, hi * 255.0, 0, 0);
            )");
            TEST("DitherQuantize spreads a between-codes value across both codes",
                !r.empty()
                && Near(r[0].x, 187.0f, 0.51f)
                && Near(r[0].y, 188.0f, 0.51f));
        }
        {
            // A gamut early-out built on this search is sound only if
            // the boundary search never reports a radius SHORTER than a
            // coarse-grid sample it would itself have found inside:
            //
            //   InTargetCube(I, dir*rq), rq on the coarse grid
            //        =>  CubeBoundaryRadius(I, dir) >= rq
            //        =>  d = r/B <= threshold  =>  SoftCompressDistance is identity
            //
            // That chain is the whole justification for skipping the search,
            // and it is why the skip rounds its test radius UP to a grid
            // sample. Testing r/threshold directly does NOT satisfy it: the
            // scan can step straight over a thin in-gamut window -- the
            // tangential vertex graze pure blue exhibits -- and that showed up
            // as 7 differing FP32 components on a real HDR frame, worst
            // 4.88e-4 scRGB. This test includes the SHIPPED helpers, the
            // header the LUT generator and the tone mapper compile, so it
            // cannot drift from the code it guards.
            const std::string pre = std::string("#include \"") + ::ShaderLab::Effects::cGamutIncludeName + "\"\n";
            const auto* desc = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindById(L"ICtCp Gamut Boundary LUT");
            TEST("Gamut boundary search HLSL is the LUT generator's include",
                 desc && desc->hlslSource.find(pre) != std::string::npos);

            // Per target gamut: the invariant is about the search, and the
            // search now runs against whichever cube the target defines. The
            // gamut arrives as a #define rather than a loop in the kernel --
            // this kernel already sits near fxc's compile-time cliff.
            const char* gamutNames[] = { "sRGB", "Display P3", "BT.2020" };
            for (int gi = 0; gi < 3; ++gi)
            {
                auto r = bench.Run(R"(
                    TargetXf tg = MakeTargetXf(TEST_GAMUT, 0, 0, 0, 0);
                    int violations = 0;
                    const float step = 0.45 / CUBE_COARSE;
                    for (int hi = 0; hi < 24; ++hi)
                    {
                        float ang = hi * (6.28318530718 / 24.0);
                        float2 dir = float2(cos(ang), sin(ang));
                        for (int ii = 1; ii <= 12; ++ii)
                        {
                            float I = ii / 13.0;
                            float peak = 6.0;          // W = 480 nits
                            float B = CubeBoundaryRadius(I, dir, peak, tg);
                            for (int k = 1; k <= CUBE_COARSE; ++k)
                            {
                                float rq = step * k;
                                if (InTargetCube(I, dir * rq, peak, tg) && B < rq - 1e-6)
                                    violations++;
                            }
                        }
                    }
                    Result[0] = float4(violations, 0, 0, 0);
                )", 1, "#define TEST_GAMUT " + std::to_string(gi) + "\n" + pre);
                TEST(std::format("CubeBoundaryRadius >= every coarse sample found inside, {} "
                                 "(the gamut early-out depends on this)", gamutNames[gi]).c_str(),
                    !r.empty() && r[0].x == 0.0f);
            }

            // Dither must be a function of the PIXEL, not of the exact float
            // D2D hands in: SCENE_POSITION carries up to 2^-12 px of
            // interpolation error that moves when D2D re-tiles an effect, and
            // IGN turned that into different codes for the same pixel.
            {
                auto r = bench.Run(R"(
                    int diffs = 0;
                    for (int y = 0; y < 32; ++y)
                        for (int x = 0; x < 32; ++x)
                        {
                            float3 v = float3(0.3137, 0.5021, 0.7412);
                            float2 c = float2(x, y) + 0.5;
                            float3 a = DitherQuantize(v, c, 256.0, 1.0);
                            float3 b = DitherQuantize(v, c + float2(4.8828125e-4, -2.44140625e-4), 256.0, 1.0);
                            if (any(a != b)) diffs++;
                        }
                    Result[0] = float4(diffs, 0, 0, 0);
                )");
                TEST("DitherQuantize is immune to sub-pixel scene-position error",
                    !r.empty() && r[0].x == 0.0f);
            }

            // Gamut Map's Clip mode routes Custom through TargetXf; its presets
            // use the library's hard-coded P3 matrix. Custom set to P3's own
            // primaries must land on the same target RGB.
            {
                auto r = bench.Run(R"(
                    TargetXf t = MakeTargetXf(3, GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B, D65_WHITE);
                    float worst = 0;
                    float3 probes[4] = { float3(1,0,0), float3(-0.5,1.2,-0.1), float3(40,-3,2), float3(0.2,0.2,0.9) };
                    for (int i = 0; i < 4; ++i)
                    {
                        float3 viaXf  = ScRGBToTarget(t, probes[i]);
                        float3 preset = mul(XYZ_TO_P3D65, ScRGBToXYZ(probes[i]));
                        float3 rel = abs(viaXf - preset) / max(abs(preset), 1e-3);
                        worst = max(worst, max(rel.x, max(rel.y, rel.z)));
                    }
                    Result[0] = float4(worst, 0, 0, 0);
                )", 1, pre);
                TEST("TargetXf Custom(P3 primaries) matches the preset P3 matrix (Gamut Map Clip)",
                    !r.empty() && r[0].x < 1e-3f);
            }

            // The target matrix itself, against published values. sRGB red in
            // P3 is (0.8225, 0.0332, 0.0171) and in BT.2020 (0.6274, 0.0691,
            // 0.0164) -- the BT.2087 / Display P3 conversion tables.
            {
                auto m = bench.Run(R"(
                    TargetXf s  = MakeTargetXf(0, 0, 0, 0, 0);
                    TargetXf p3 = MakeTargetXf(1, 0, 0, 0, 0);
                    TargetXf bt = MakeTargetXf(2, 0, 0, 0, 0);
                    // Custom set to P3's own primaries and D65: must equal preset 1.
                    TargetXf cp = MakeTargetXf(3, GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B, D65_WHITE);
                    // Custom P3 primaries with a D50 white: a D65 neutral must land
                    // as R = G = B (the adaptation's whole job).
                    TargetXf d5 = MakeTargetXf(3, GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B, float2(0.3457, 0.3585));
                    float3 red = float3(1, 0, 0);
                    Result[0] = float4(ScRGBToTarget(p3, red), 0);
                    Result[1] = float4(ScRGBToTarget(bt, red), 0);
                    float3 x = float3(0.3, -0.2, 1.7);
                    Result[2] = float4(TargetToScRGB(p3, ScRGBToTarget(p3, x)) - x,
                                       length(TargetToScRGB(d5, ScRGBToTarget(d5, x)) - x));
                    Result[3] = float4(s.fingerprint, p3.fingerprint, bt.fingerprint, cp.fingerprint);
                    Result[4] = float4(ScRGBToTarget(d5, float3(2, 2, 2)), s.identity ? 1 : 0);
                    // Luminance weights: a target-RGB white must read as Y = 1.
                    Result[5] = float4(dot(float3(1,1,1), p3.lumaT), dot(float3(1,1,1), bt.lumaT),
                                       dot(float3(1,1,1), d5.lumaT), 0);
                )", 6, pre);
                const bool ok = m.size() >= 6;
                TEST("TargetXf: sRGB red -> P3 matches (0.8225, 0.0332, 0.0171)",
                    ok && Near(m[0].x, 0.8225f, 2e-3f) && Near(m[0].y, 0.0332f, 2e-3f) && Near(m[0].z, 0.0171f, 2e-3f));
                TEST("TargetXf: sRGB red -> BT.2020 matches (0.6274, 0.0691, 0.0164)",
                    ok && Near(m[1].x, 0.6274f, 2e-3f) && Near(m[1].y, 0.0691f, 2e-3f) && Near(m[1].z, 0.0164f, 2e-3f));
                TEST("TargetXf: forward/inverse round-trip (D65 and adapted D50)",
                    ok && std::abs(m[2].x) < 1e-4f && std::abs(m[2].y) < 1e-4f && std::abs(m[2].z) < 1e-4f && m[2].w < 1e-4f);
                TEST("TargetXf: sRGB is the exact identity (fingerprint 15)",
                    ok && m[4].w == 1.0f && m[3].x == 15.0f);
                TEST("TargetXf: Custom with P3 primaries + D65 fingerprints as the P3 preset",
                    ok && std::abs(m[3].w - m[3].y) <= 1e-4f * m[3].y);
                TEST("TargetXf: preset fingerprints are far apart (LUT stamp can tell them apart)",
                    ok && std::abs(m[3].x - m[3].y) > 1e-2f && std::abs(m[3].y - m[3].z) > 1e-2f
                       && std::abs(m[3].x - m[3].z) > 1e-2f);
                TEST("TargetXf: Bradford D65 -> D50 keeps a neutral neutral (R = G = B)",
                    ok && Near(m[4].x, m[4].y, 1e-4f * 2) && Near(m[4].y, m[4].z, 1e-4f * 2));
                TEST("TargetXf: target white reads as luminance 1 (P3, BT.2020, D50-adapted)",
                    ok && Near(m[5].x, 1.0f, 1e-4f) && Near(m[5].y, 1.0f, 1e-4f) && Near(m[5].z, 1.0f, 1e-4f));
            }

            // DCI-P3 (gamut 4): P3 primaries under the DCI white, adapted to
            // D65. Its normalised primary matrix is SMPTE RP 431-2's, and
            // adaptation makes DCI white (1,1,1) land on scRGB white.
            {
                auto m = bench.Run(R"(
                    TargetXf dci = MakeTargetXf(4, 0, 0, 0, 0);
                    TargetXf p3  = MakeTargetXf(1, 0, 0, 0, 0);
                    TargetXf s   = MakeTargetXf(0, 0, 0, 0, 0);
                    bool ok;
                    float3x3 npm = RgbToXyzFromPrimaries(GAMUT_P3_R, GAMUT_P3_G, GAMUT_P3_B, DCI_WHITE, ok);
                    Result[0] = float4(npm[0], ok ? 1 : 0);
                    Result[1] = float4(npm[1], 0);
                    Result[2] = float4(npm[2], 0);
                    Result[3] = float4(TargetToScRGB(dci, float3(1, 1, 1)), dci.identity ? 1 : 0);
                    Result[4] = float4(ScRGBToTarget(dci, float3(3, 3, 3)), 0);
                    Result[5] = float4(dci.fingerprint, p3.fingerprint, s.fingerprint, dot(float3(1,1,1), dci.lumaT));
                    // Each primary's chromaticity in the working space, and the
                    // library's constants for it.
                    for (int i = 0; i < 3; ++i)
                    {
                        float3 xyz = ScRGBToXYZ(TargetToScRGB(dci, float3(i == 0, i == 1, i == 2)));
                        float2 constant = (i == 0) ? GAMUT_DCIP3_R : (i == 1) ? GAMUT_DCIP3_G : GAMUT_DCIP3_B;
                        Result[6 + i] = float4(xyz.xy / (xyz.x + xyz.y + xyz.z), constant);
                    }
                    Result[9] = float4(ScRGBToTarget(dci, float3(1, 0, 0)), 0);
                )", 10, pre);
                const bool ok = m.size() >= 10 && m[0].w == 1.0f;
                TEST("DCI-P3: RGB->XYZ under DCI white matches SMPTE RP 431-2",
                    ok && Near(m[0].x, 0.4451698f, 1e-5f) && Near(m[0].y, 0.2771344f, 1e-5f) && Near(m[0].z, 0.1722827f, 1e-5f)
                       && Near(m[1].x, 0.2094917f, 1e-5f) && Near(m[1].y, 0.7215953f, 1e-5f) && Near(m[1].z, 0.0689131f, 1e-5f)
                       && Near(m[2].x, 0.0f, 1e-5f)       && Near(m[2].y, 0.0470606f, 1e-5f) && Near(m[2].z, 0.9073554f, 1e-5f));
                TEST("DCI-P3: adapted, DCI white (1,1,1) is scRGB white and scRGB white stays neutral",
                    ok && m[3].w == 0.0f
                       && Near(m[3].x, 1.0f, 1e-4f) && Near(m[3].y, 1.0f, 1e-4f) && Near(m[3].z, 1.0f, 1e-4f)
                       && Near(m[4].x, 3.0f, 3e-4f) && Near(m[4].y, 3.0f, 3e-4f) && Near(m[4].z, 3.0f, 3e-4f)
                       && Near(m[5].w, 1.0f, 1e-4f));
                // Worked in double: sRGB red in adapted DCI-P3 is (0.86858, 0.03454, 0.01677).
                TEST("DCI-P3: sRGB red -> adapted DCI-P3 matches (0.8686, 0.0345, 0.0168)",
                    ok && Near(m[9].x, 0.86858f, 2e-4f) && Near(m[9].y, 0.03454f, 2e-4f) && Near(m[9].z, 0.01677f, 2e-4f));
                // The stamp check allows 1e-5 of the fingerprint; sRGB is the nearest preset.
                TEST("DCI-P3: fingerprint tells it from Display P3 and sRGB (LUT stamp)",
                    ok && std::abs(m[5].x - m[5].y) > 1e-2f
                       && std::abs(m[5].x - m[5].z) > 5.0f * 1e-5f * m[5].z);
                bool constantsMatch = ok;
                bool cpuMatches = ok;
                const auto cpu = ShaderLab::Effects::ColorMathCpu::GamutPrimaries(4, {}, {}, {});
                for (int i = 0; i < 3 && ok; ++i)
                {
                    constantsMatch = constantsMatch && Near(m[6 + i].x, m[6 + i].z, 2e-6f) && Near(m[6 + i].y, m[6 + i].w, 2e-6f);
                    cpuMatches = cpuMatches && Near(static_cast<float>(cpu[i].x), m[6 + i].x, 2e-6f)
                                            && Near(static_cast<float>(cpu[i].y), m[6 + i].y, 2e-6f);
                }
                printf("  [info] DCI-P3 adapted primaries: R (%.6f, %.6f) G (%.6f, %.6f) B (%.6f, %.6f)\n",
                       m.size() >= 10 ? m[6].x : 0.0f, m.size() >= 10 ? m[6].y : 0.0f,
                       m.size() >= 10 ? m[7].x : 0.0f, m.size() >= 10 ? m[7].y : 0.0f,
                       m.size() >= 10 ? m[8].x : 0.0f, m.size() >= 10 ? m[8].y : 0.0f);
                TEST("DCI-P3: GAMUT_DCIP3_* are the primaries MakeTargetXf(4) implies", constantsMatch);
                TEST("DCI-P3: ColorMathCpu::GamutPrimaries(4) matches the HLSL", cpuMatches);
            }

            // Table ACCURACY is deliberately not measured here. An in-kernel
            // emulation (texels computed by CubeBoundaryRadius, blended with the
            // reader's index math) sent fxc past 2.5 GB and minutes of compile
            // even with one call site and 7x fewer queries -- dynamic indexing
            // and a data-dependent branch inside a [loop] wrapping the search's
            // 27 unrolled reconstructions. It is measured end to end instead,
            // on the real D2D texture path: see the CHANGELOG entry for the
            // ICtCp Gamut Boundary LUT.
        }
        {
            // Mean preservation is the entire point of dithering: averaged over
            // a field, the quantized output must land back on the un-quantized
            // value. A fractional part away from .5 is the discriminating case.
            // An exact half-code -- like the 187.5 above -- is mean-preserving
            // even at HALF the correct amplitude, which is exactly why that
            // test did not catch a half-amplitude dither: the shipped
            // DitherQuantize scaled by 0.5*strength and left ~0.197 codes of
            // bias at every other fractional part.
            auto r = bench.Run(R"(
                float s0 = 0, s1 = 0;
                for (int y = 0; y < 16; ++y) {
                    for (int x = 0; x < 16; ++x) {
                        float2 p = float2(x, y);
                        s0 += DitherQuantize(float3(200.75 / 255.0, 0, 0), p, 256.0, 1.0).x;
                        s1 += DitherQuantize(float3(254.7083 / 255.0, 0, 0), p, 256.0, 1.0).x;
                    }
                }
                Result[0] = float4(s0 / 256.0 * 255.0, s1 / 256.0 * 255.0, 0, 0);
            )");
            TEST("DitherQuantize(strength 1) is mean-preserving off a half-code",
                !r.empty()
                && Near(r[0].x, 200.75f, 0.05f)
                && Near(r[0].y, 254.7083f, 0.05f));
        }
    }
}

namespace ShaderLab::Tests
{
    namespace
    {
        SL_REGISTER_TEST(TestSrgbTransfer, Tag::Gpu | Tag::Math, [] { TestSrgbTransfer(Bench()); });
        SL_REGISTER_TEST(TestXyYConversions, Tag::Gpu | Tag::Math, [] { TestXyYConversions(Bench()); });
        SL_REGISTER_TEST(TestPointInTriangle, Tag::Gpu | Tag::Math, [] { TestPointInTriangle(Bench()); });
        SL_REGISTER_TEST(TestOklab, Tag::Gpu | Tag::Math, [] { TestOklab(Bench()); });
        SL_REGISTER_TEST(TestScRgbToLabAnchor, Tag::Gpu | Tag::Math, [] { TestScRgbToLabAnchor(Bench()); });
        SL_REGISTER_TEST(TestSoftCompressDistance, Tag::Gpu | Tag::Math, [] { TestSoftCompressDistance(Bench()); });
        SL_REGISTER_TEST(TestDitherQuantize, Tag::Gpu | Tag::Math, [] { TestDitherQuantize(Bench()); });
    }
}
