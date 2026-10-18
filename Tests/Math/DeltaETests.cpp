#include "pch_engine.h"
#include "../ShaderTestBench.h"
#include "../TestSupport.h"
#include "../../Effects/ShaderLabEffects.h"

#include <cstdio>
#include <string>

namespace ShaderLab::Tests
{
    // Every metric here is the shipped one from shaderlab_colormath.hlsli,
    // which the bench includes and the Delta E Comparator calls.
    void TestDeltaE(ShaderTestBench& bench)
    {
        std::printf("\n=== Math: Delta E ===\n");

        {
            const auto* comparator = ::ShaderLab::Effects::ShaderLabEffects::Instance().FindByName(L"Delta E Comparator");
            TEST("Delta E Comparator uses the shared Lab metrics",
                 comparator && comparator->hlslSource.find("#include \"shaderlab_colormath.hlsli\"") != std::string::npos &&
                 comparator->hlslSource.find("CieDeltaE2000(") != std::string::npos &&
                 comparator->hlslSource.find("float DeltaE2000(") == std::string::npos);
        }

        // ---- Identity & symmetry ------------------------------------------
        {
            auto r = bench.Run(R"(
                float3 lab = float3(50.0, 10.0, -5.0);
                float d76 = CieDeltaE76(lab, lab);
                float d94 = CieDeltaE94(lab, lab);
                float d2000 = CieDeltaE2000(lab, lab);
                Result[0] = float4(d76, d94, d2000, 0);
            )", 1);
            TEST("CieDeltaE76(c, c) == 0",
                !r.empty() && Near(r[0].x, 0.0f, 1e-5f));
            TEST("CieDeltaE94(c, c) == 0",
                !r.empty() && Near(r[0].y, 0.0f, 1e-5f));
            TEST("CieDeltaE2000(c, c) == 0",
                !r.empty() && Near(r[0].z, 0.0f, 1e-5f));
        }
        {
            auto r = bench.Run(R"(
                float3 a = float3(50.0, 10.0, -5.0);
                float3 b = float3(45.0, -3.0, 8.0);
                float dab = CieDeltaE76(a, b);
                float dba = CieDeltaE76(b, a);
                Result[0] = float4(dab, dba, abs(dab - dba), 0);
            )", 1);
            TEST("DeltaE76 is symmetric: dE(a,b) == dE(b,a)",
                !r.empty() && Near(r[0].z, 0.0f, 1e-4f));
        }

        // ---- DeltaE76 algebra ---------------------------------------------
        // Pure Euclidean L*a*b* distance — easy to compute by hand.
        // (50,0,0) <-> (40,3,-4): dL=-10, da=3, db=-4. dE = sqrt(100+9+16)=sqrt(125)~=11.180.
        {
            auto r = bench.Run(R"(
                float d = CieDeltaE76(float3(50, 0, 0), float3(40, 3, -4));
                Result[0] = float4(d, 0, 0, 0);
            )", 1);
            TEST("CieDeltaE76((50,0,0),(40,3,-4)) ~= 11.180 (sqrt(125))",
                !r.empty() && Near(r[0].x, 11.180f, 1e-2f));
        }

        // ---- Delta E ITP (BT.2124) ----------------------------------------
        {
            auto r = bench.Run(R"(
                float3 c = float3(0.5, 0.02, -0.03);
                float3 a = float3(0.5, 0.02, -0.03);
                float3 b = float3(0.4, -0.01, 0.05);
                float ident = DeltaEITP(c, c);
                float dab = DeltaEITP(a, b);
                float dba = DeltaEITP(b, a);
                Result[0] = float4(ident, dab, dba, abs(dab - dba));
            )", 1);
            TEST("DeltaEITP(c, c) == 0", !r.empty() && Near(r[0].x, 0.0f, 1e-6f));
            TEST("DeltaEITP is symmetric", !r.empty() && Near(r[0].w, 0.0f, 1e-5f));
        }
        {
            // The 720 scale factor, isolated: a pure I difference of 0.001
            // must read 0.72 by definition of the metric.
            auto r = bench.Run(R"(
                float d = DeltaEITP(float3(0.0, 0.0, 0.0), float3(0.001, 0.0, 0.0));
                Result[0] = float4(d, 0, 0, 0);
            )", 1);
            TEST("DeltaEITP scale: dI=0.001 -> 0.72", !r.empty() && Near(r[0].x, 0.72f, 1e-4f));
        }
        {
            // BT.2124's defining asymmetry: T = 0.5*Ct while P = Cp, so an
            // identical numeric step in Cp counts DOUBLE the same step in Ct.
            // Dropping the 0.5 (or applying it to Cp) is the easy bug.
            auto r = bench.Run(R"(
                float dCt = DeltaEITP(float3(0,0,0), float3(0, 0.01, 0));
                float dCp = DeltaEITP(float3(0,0,0), float3(0, 0, 0.01));
                Result[0] = float4(dCt, dCp, dCp / max(dCt, 1e-9), 0);
            )", 1);
            TEST("DeltaEITP weights Ct at half of Cp (ratio == 2)",
                !r.empty() && Near(r[0].z, 2.0f, 1e-4f));
            TEST("DeltaEITP Ct step: 0.01 -> 3.6",
                !r.empty() && Near(r[0].x, 3.6f, 1e-3f));
        }
        {
            // Grounds the scale against a metric that IS valid in the SDR
            // sRGB domain: for a small near-neutral step both are in their
            // fitted range and ~1 unit = ~1 JND, so they must agree within a
            // small factor. This is what would catch a wrong constant
            // (720 vs 100 vs 1) that the pure-formula checks accept happily.
            auto r = bench.Run(R"(
                float3 rgbA = float3(0.5, 0.5, 0.5);
                float3 rgbB = float3(0.52, 0.5, 0.5);
                float itp  = DeltaEITPFromScRGB(rgbA, rgbB);
                float lab  = CieDeltaE2000(ScRGBToLab(rgbA), ScRGBToLab(rgbB));
                Result[0] = float4(itp, lab, itp / max(lab, 1e-6), 0);
            )", 1);
            bool sane = !r.empty() && r[0].x > 0.0f && r[0].y > 0.0f &&
                        r[0].z > 0.2f && r[0].z < 5.0f;
            if (!r.empty())
                std::printf("  dE ITP %.3f vs dE2000 %.3f (ratio %.2f) on a small SDR step\n",
                            r[0].x, r[0].y, r[0].z);
            TEST("DeltaEITP magnitude agrees with dE2000 within 5x on a small SDR step", sane);
        }
        {
            // Monotonic in separation, and meaningful where the Lab metrics
            // are out of their domain: bright HDR neutrals.
            // (scRGB 1.0 = 80 nits, so 12.5 = 1000 nits.)
            auto r = bench.Run(R"(
                float3 a = float3(12.5, 12.5, 12.5);
                float3 b = float3(13.75, 13.75, 13.75);
                float3 c = float3(25.0, 25.0, 25.0);
                float dNear = DeltaEITPFromScRGB(a, b);
                float dFar  = DeltaEITPFromScRGB(a, c);
                Result[0] = float4(dNear, dFar, dFar - dNear, 0);
            )", 1);
            TEST("DeltaEITP is non-zero at HDR levels (1000 vs 1100 nits)",
                !r.empty() && r[0].x > 0.5f);
            TEST("DeltaEITP grows with separation at HDR levels",
                !r.empty() && r[0].z > 0.0f);
        }

        // ---- DeltaE2000 Sharma reference pairs ----------------------------
        // From Sharma, Wu, Dalal: "The CIEDE2000 Color-Difference Formula:
        // Implementation Notes, Supplementary Test Data, and Mathematical
        // Observations" (Color Research and Application, 2005, table 1).
        // Use a few well-spread pairs across the L/C/h space to exercise
        // every branch of the formula (G correction, hue wrap, T term,
        // RT rotation in the blue region).
        //
        // Pair 6 (50,-1,2) vs (50,0,0) has a2 = b2 = 0, where atan2(0,0)
        // would put NaN into hp_avg; the shader guards h2p with
        // C2p < 1e-10.
        struct Pair { float l1, a1, b1, l2, a2, b2, expected; };
        const Pair pairs[] = {
            { 50.0000f,  2.6772f, -79.7751f, 50.0000f, 0.0000f, -82.7485f, 2.0425f },
            { 50.0000f, -1.0000f,   2.0000f, 50.0000f, 0.0000f,   0.0000f, 2.3669f },
            { 50.0000f,  2.5000f,   0.0000f, 50.0000f, 0.0000f,  -2.5000f, 4.3065f },
            { 60.2574f, -34.0099f, 36.2677f, 60.4626f, -34.1751f, 39.4387f, 1.2644f },
            { 22.7233f,  20.0904f, -46.6940f, 23.0331f, 14.9730f, -42.5619f, 2.0373f },
        };
        for (const auto& p : pairs)
        {
            char body[512];
            std::snprintf(body, sizeof(body),
                "float d = CieDeltaE2000(float3(%.4f,%.4f,%.4f), float3(%.4f,%.4f,%.4f));"
                "Result[0] = float4(d, 0, 0, 0);",
                p.l1, p.a1, p.b1, p.l2, p.a2, p.b2);
            auto r = bench.Run(std::string(body), 1);
            char name[256];
            std::snprintf(name, sizeof(name),
                "DeltaE2000 Sharma pair (%.2f, %.2f, %.2f) vs (%.2f, %.2f, %.2f) ~= %.4f",
                p.l1, p.a1, p.b1, p.l2, p.a2, p.b2, p.expected);
            TEST(name, !r.empty() && Near(r[0].x, p.expected, 5e-3f));
        }
    }
}

namespace ShaderLab::Tests
{
    namespace
    {
        SL_REGISTER_TEST(TestDeltaE, Tag::Gpu | Tag::Math, [] { TestDeltaE(Bench()); });
    }
}
