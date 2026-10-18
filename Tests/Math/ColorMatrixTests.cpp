#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestSupport.h"
#include "Effects/ColorMathCpu.h"

#include <cstdio>

namespace ShaderLab::Tests
{
    void TestColorMatrices(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: Color Matrices ===\n");

        // ---- scRGB <-> XYZ ------------------------------------------------
        // scRGB(1,1,1) -> XYZ should land on the matrix's chosen D65. The
        // codebase's REC709_TO_XYZ matrix sums to (0.9504559, 1.0, 1.0890580)
        // for input (1,1,1) — slightly different from the textbook D65
        // (0.95047, 1.0, 1.08883) because the matrix is derived from the
        // exact CIE xy chromaticities rather than tabulated XYZ. Test
        // against the algebraic value, not the textbook one.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToXYZ(float3(1.0, 1.0, 1.0)), 0);
            )");
            TEST("ScRGBToXYZ(1,1,1) ~= (0.9505, 1.0, 1.0891) algebraic D65",
                !r.empty()
                && Near(r[0].x, 0.9504559f, 5e-4f)
                && Near(r[0].y, 1.0000000f, 1e-5f)
                && Near(r[0].z, 1.0890580f, 5e-4f));
        }
        // Inverse: XYZ(matrix-D65) -> ScRGB should land on (1,1,1).
        {
            auto r = bench.Run(R"(
                Result[0] = float4(XYZToScRGB(float3(0.9504559, 1.0, 1.0890580)), 0);
            )");
            TEST("XYZToScRGB(matrix-D65) ~= (1, 1, 1)",
                !r.empty()
                && Near(r[0].x, 1.0f, 1e-4f)
                && Near(r[0].y, 1.0f, 1e-4f)
                && Near(r[0].z, 1.0f, 1e-4f));
        }
        // ScRGB(0,0,0) -> XYZ(0,0,0).
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToXYZ(float3(0,0,0)), 0);
            )");
            TEST("ScRGBToXYZ(0) == 0",
                !r.empty()
                && Near(r[0].x, 0.0f, 1e-6f)
                && Near(r[0].y, 0.0f, 1e-6f)
                && Near(r[0].z, 0.0f, 1e-6f));
        }
        // Inverse-matrix product: XYZ_TO_REC709 * REC709_TO_XYZ ~= I.
        // Test by routing the three basis vectors through both matrices.
        {
            auto r = bench.Run(R"(
                float3 x = XYZToScRGB(ScRGBToXYZ(float3(1,0,0)));
                float3 y = XYZToScRGB(ScRGBToXYZ(float3(0,1,0)));
                float3 z = XYZToScRGB(ScRGBToXYZ(float3(0,0,1)));
                // Reduce to a single max-deviation scalar.
                float3 ex = x - float3(1,0,0);
                float3 ey = y - float3(0,1,0);
                float3 ez = z - float3(0,0,1);
                float maxErr = max(max(
                    max(max(abs(ex.x), abs(ex.y)), abs(ex.z)),
                    max(max(abs(ey.x), abs(ey.y)), abs(ey.z))),
                    max(max(abs(ez.x), abs(ez.y)), abs(ez.z)));
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("XYZ_TO_REC709 * REC709_TO_XYZ ~= I (max basis err < 1e-5)",
                !r.empty() && r[0].x < 1e-5f);
        }

        // ---- 6-color round trip through XYZ -------------------------------
        {
            auto r = bench.Run(R"(
                float3 colors[6] = {
                    float3(1, 0, 0),       // primary R
                    float3(0, 1, 0),       // primary G
                    float3(0, 0, 1),       // primary B
                    float3(0.5, 0.5, 0.5), // gray
                    float3(0.18, 0.18, 0.18), // 18% gray
                    float3(0.9, 0.7, 0.6)  // skin tone
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 6; ++i) {
                    float3 rt = XYZToScRGB(ScRGBToXYZ(colors[i]));
                    float3 d = abs(rt - colors[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("ScRGB <-> XYZ round trip on 6-color suite (max err < 1e-5)",
                !r.empty() && r[0].x < 1e-5f);
        }

        // ---- scRGB <-> ICtCp ----------------------------------------------
        // Analytic anchor: scRGB(1,1,1) is 80-nit neutral white. Through
        // ScRGBToXYZ * 80 -> (76.04, 80, 87.13). XYZ_TO_LMS_ICTCP picks
        // exactly equal LMS components (~80 each, by construction). After
        // PQ_InvEOTF(80), PQLMS_TO_ICTCP first row sums to 1.0 * pq for I,
        // and the Ct/Cp rows sum to zero on neutrals. So ICtCp(white) is
        // (PQ_InvEOTF(80nits), 0, 0) ~= (0.498, 0, 0). Tolerance loose on
        // the I component since it follows PQ_InvEOTF's curve exactly.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToICtCp(float3(1, 1, 1)), 0);
            )");
            TEST("ScRGBToICtCp(white@80nits) ~= (PQ_InvEOTF(80), 0, 0); chroma channels neutral",
                !r.empty()
                && Near(r[0].x, 0.498f, 2e-2f)
                && Near(r[0].y, 0.0f,    1e-4f)
                && Near(r[0].z, 0.0f,    1e-4f));
        }
        // The CPU port (Effects/ColorMathCpu.h) builds tables the ICtCp
        // shaders read instead of recomputing per pixel, so it must agree
        // with the HLSL it copies. Colours include wide-gamut negatives and
        // HDR; the boundary vertex exercises PQ_EOTF + XYZToScRGB too.
        {
            auto r = bench.Run(R"(
                Result[0] = float4(ScRGBToICtCp(float3(1, 1, 1)), 0);
                Result[1] = float4(ScRGBToICtCp(float3(-0.87, 1.0, 0.06)), 0);
                Result[2] = float4(ScRGBToICtCp(float3(12.5, 0.3, 0.02)), 0);
                Result[3] = float4(ScRGBToICtCp(float3(0.01, 0.02, 0.9)), 0);
                // SampleBoundary's vertex 7 of 48 on BT.709 at I = 0.55.
                float nits = PQ_EOTF(0.55);
                float Ys = max(nits / 80.0, 0.0001);
                float2 xy = lerp(GAMUT_709_R, GAMUT_709_G, 7.0 / 16.0);
                float3 ic = ScRGBToICtCp(XYZToScRGB(float3(xy.x * Ys / xy.y, Ys, (1.0 - xy.x - xy.y) * Ys / xy.y)));
                Result[4] = float4(ic.y, ic.z, 0, 0);
            )", 5);
            namespace CM = ShaderLab::Effects::ColorMathCpu;
            const CM::V3 in[4] = { { 1, 1, 1 }, { -0.87, 1.0, 0.06 }, { 12.5, 0.3, 0.02 }, { 0.01, 0.02, 0.9 } };
            float maxErr = r.size() >= 5 ? 0.0f : 1.0f;
            for (int i = 0; i < 4 && r.size() >= 5; ++i)
            {
                const CM::V3 c = CM::ScRGBToICtCp(in[i]);
                maxErr = (std::max)(maxErr, static_cast<float>(std::abs(c.x - r[i].x)));
                maxErr = (std::max)(maxErr, static_cast<float>(std::abs(c.y - r[i].y)));
                maxErr = (std::max)(maxErr, static_cast<float>(std::abs(c.z - r[i].z)));
            }
            if (r.size() >= 5)
            {
                std::array<CM::V2, 48> b{};
                CM::SampleBoundary(CM::GamutPrimaries(0, {}, {}, {}), 0.55, b);
                maxErr = (std::max)(maxErr, static_cast<float>(std::abs(b[7].x - r[4].x)));
                maxErr = (std::max)(maxErr, static_cast<float>(std::abs(b[7].y - r[4].y)));
            }
            printf("  [info] ColorMathCpu vs HLSL max abs err %.3g\n", maxErr);
            TEST("ColorMathCpu matches the HLSL ICtCp path (max abs err < 1e-4)", maxErr < 1e-4f);
        }
        // Inverse: ICtCp(0.498, 0, 0) -> scRGB white. Use the same I value
        // we expect from the forward direction so the round trip is exact.
        {
            auto r = bench.Run(R"(
                float I = ScRGBToICtCp(float3(1, 1, 1)).x;
                Result[0] = float4(ICtCpToScRGB(float3(I, 0, 0)), 0);
            )");
            TEST("ICtCpToScRGB(forward(white)) ~= (1, 1, 1) [round trip on neutral]",
                !r.empty()
                && Near(r[0].x, 1.0f, 5e-3f)
                && Near(r[0].y, 1.0f, 5e-3f)
                && Near(r[0].z, 1.0f, 5e-3f));
        }
        // ScRGB <-> ICtCp 6-color round trip. Tolerance loose: PQ encode
        // at small scRGB values has more relative quantization error.
        {
            auto r = bench.Run(R"(
                float3 colors[6] = {
                    float3(1, 0, 0),
                    float3(0, 1, 0),
                    float3(0, 0, 1),
                    float3(1, 1, 1),
                    float3(0.5, 0.5, 0.5),
                    float3(0.9, 0.7, 0.6)
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 6; ++i) {
                    float3 rt = ICtCpToScRGB(ScRGBToICtCp(colors[i]));
                    float3 d = abs(rt - colors[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("ScRGB <-> ICtCp round trip on 6-color suite (max err < 5e-3)",
                !r.empty() && r[0].x < 5e-3f);
        }

        // ---- Wide-gamut (negative scRGB) survival --------------------------
        // scRGB carries wide-gamut colour as negative Rec.709 components, so
        // the ICtCp round trip must preserve them. A `max(rgb, 0)` on entry
        // would silently sRGB-clip every wide-gamut pixel before any
        // tone/gamut mapping ran.
        {
            // BT.2020 and DCI-P3 primaries at 80 nits, expressed in scRGB.
            // Each has at least one strongly negative component.
            auto r = bench.Run(R"(
                float3 wide[4] = {
                    float3(-0.8667,  1.0000,  0.0596),   // BT.2020 green-ish
                    float3( 1.2484, -0.0479, -0.0184),   // BT.2020 red-ish
                    float3(-0.1067,  1.0128,  0.0294),   // P3 green-ish
                    float3( 1.0930, -0.2267,  0.0442)    // P3 red-ish
                };
                float maxErr = 0.0;
                [unroll]
                for (int i = 0; i < 4; ++i) {
                    float3 rt = ICtCpToScRGB(ScRGBToICtCp(wide[i]));
                    float3 d = abs(rt - wide[i]);
                    maxErr = max(maxErr, max(max(d.x, d.y), d.z));
                }
                Result[0] = float4(maxErr, 0, 0, 0);
            )");
            TEST("ICtCp round trip preserves wide-gamut negatives (max err < 5e-3)",
                !r.empty() && r[0].x < 5e-3f);
        }
        {
            // A negative component must not collapse to zero. Pin the sign
            // and rough magnitude so a clamp fails loudly rather than
            // drifting.
            auto r = bench.Run(R"(
                float3 rt = ICtCpToScRGB(ScRGBToICtCp(float3(-0.8667, 1.0, 0.0596)));
                Result[0] = float4(rt, 0);
            )");
            TEST("ICtCp round trip keeps R strongly negative (no sRGB clip on entry)",
                !r.empty() && r[0].x < -0.80f && r[0].x > -0.93f);
        }
        {
            // Signed PQ is symmetric about the origin, so below-black input
            // round-trips rather than pinning to zero.
            auto r = bench.Run(R"(
                float3 rt = ICtCpToScRGB(ScRGBToICtCp(float3(-0.1, -0.1, -0.1)));
                Result[0] = float4(rt, 0);
            )");
            TEST("ICtCp round trip preserves below-black neutral (-0.1 stays negative)",
                !r.empty() && r[0].x < -0.09f && r[0].x > -0.11f);
        }
        {
            // A non-finite texel must not leave ICtCpToScRGB at roughly
            // -10000 nits (scRGB -125): clamp() with a NaN operand need not
            // pick either bound, and a -1 there is what PQ_EOTF_Signed
            // expands. One such texel from an HDR swapchain would poison
            // every downstream mean, max and dE. Bit patterns, not 0.0/0.0,
            // so the optimizer cannot fold the inputs away at compile time.
            auto r = bench.Run(R"(
                float qnan = asfloat(0x7FC00000u);
                float pinf = asfloat(0x7F800000u);
                float ninf = asfloat(0xFF800000u);
                float3 a = ICtCpToScRGB(float3(qnan, 0.0, 0.0));
                float3 b = ICtCpToScRGB(float3(0.5, pinf, ninf));
                // Largest magnitude seen across both results. The old failure
                // mode reads ~125 here; a correct guard keeps it small.
                float worst = max(max(max(abs(a.x), abs(a.y)), abs(a.z)),
                                  max(max(abs(b.x), abs(b.y)), abs(b.z)));
                Result[0] = float4(worst, a.x, b.x, 0);
            )");
            TEST("ICtCpToScRGB does not explode on non-finite input",
                !r.empty() && r[0].x < 2.0f);
            // And it must not smuggle a non-finite value out either.
            TEST("ICtCpToScRGB returns finite values for non-finite input",
                !r.empty() && std::isfinite(r[0].y) && std::isfinite(r[0].z));
        }
        {
            // IsNonFinite must survive the optimizer. Compiled with
            // OPTIMIZATION_LEVEL3 and no IEEE strictness, isfinite() and any
            // magnitude comparison against NaN are foldable; the bit test is
            // not. If this ever reads 0 for the NaN/Inf cases, the guard above
            // has been optimized out and the -10000 nit failure is back.
            auto r = bench.Run(R"(
                Result[0] = float4(
                    IsNonFinite(asfloat(0x7FC00000u)) ? 1.0 : 0.0,
                    IsNonFinite(asfloat(0x7F800000u)) ? 1.0 : 0.0,
                    IsNonFinite(asfloat(0xFF800000u)) ? 1.0 : 0.0,
                    IsNonFinite(1.5) ? 1.0 : 0.0);
            )");
            TEST("IsNonFinite detects NaN / +Inf / -Inf and passes finite",
                !r.empty() && r[0].x == 1.0f && r[0].y == 1.0f
                           && r[0].z == 1.0f && r[0].w == 0.0f);
        }
    }
}

namespace ShaderLab::Tests
{
    namespace
    {
        SL_REGISTER_TEST(TestColorMatrices, Tag::Gpu | Tag::Math, [] { TestColorMatrices(Bench()); });
    }
}
