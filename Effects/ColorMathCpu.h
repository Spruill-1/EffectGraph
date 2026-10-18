#pragma once

#include <array>
#include <cmath>

// CPU port of the parts of the ColorMath HLSL prelude (Effects/ColorMath.cpp)
// that host-side "derived constants" need: scRGB <-> XYZ, the PQ curve and
// scRGB -> ICtCp, plus the gamut-boundary sampling several ICtCp effects do.
//
// Why it exists: a pixel shader cannot share work between pixels, so a
// quantity that depends only on uniforms -- a gamut boundary polygon, a fit
// scale per intensity -- was being rebuilt for EVERY pixel (ICtCp Gamut Map's
// Fit to Shell measured 1.9 s per frame at 1 Mpx). Those tables are now built
// here once per parameter change and uploaded as cbuffer arrays.
//
// Constants are copied verbatim from the HLSL. Tests/Math/ColorMatrixTests.cpp
// compares this port against the shader on the GPU, so the two cannot drift
// silently.
// Math is in double; the HLSL is float, so expect ~1e-6 relative agreement.
namespace ShaderLab::Effects::ColorMathCpu
{
    struct V2 { double x{ 0 }, y{ 0 }; };
    struct V3 { double x{ 0 }, y{ 0 }, z{ 0 }; };

    inline V3 Mul(const double m[9], V3 v)
    {
        return { m[0] * v.x + m[1] * v.y + m[2] * v.z,
                 m[3] * v.x + m[4] * v.y + m[5] * v.z,
                 m[6] * v.x + m[7] * v.y + m[8] * v.z };
    }

    inline constexpr double REC709_TO_XYZ[9] = {
        0.4123908, 0.3575843, 0.1804808,
        0.2126390, 0.7151687, 0.0721923,
        0.0193308, 0.1191950, 0.9505322 };
    inline constexpr double XYZ_TO_REC709[9] = {
         3.2409699, -1.5373832, -0.4986108,
        -0.9692436,  1.8759675,  0.0415551,
         0.0556301, -0.2039770,  1.0569715 };
    inline constexpr double XYZ_TO_LMS_ICTCP[9] = {
         0.3592832, 0.6976051, -0.0358916,
        -0.1920808, 1.1004768,  0.0753741,
         0.0070797, 0.0748262,  0.8433009 };
    inline constexpr double PQLMS_TO_ICTCP[9] = {
         2048.0 / 4096.0,   2048.0 / 4096.0,     0.0 / 4096.0,
         6610.0 / 4096.0, -13613.0 / 4096.0,  7003.0 / 4096.0,
        17933.0 / 4096.0, -17390.0 / 4096.0,  -543.0 / 4096.0 };

    // Gamut primaries (CIE xy), as GAMUT_* in the HLSL.
    inline constexpr V2 GAMUT_709[3]  = { { 0.64, 0.33 },   { 0.30, 0.60 },   { 0.15, 0.06 } };
    inline constexpr V2 GAMUT_P3[3]   = { { 0.680, 0.320 }, { 0.265, 0.690 }, { 0.150, 0.060 } };
    inline constexpr V2 GAMUT_2020[3] = { { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 } };
    // DCI-P3 Bradford-adapted to D65 (GAMUT_DCIP3_* in the HLSL).
    inline constexpr V2 GAMUT_DCIP3[3] = { { 0.680701, 0.318895 }, { 0.281207, 0.674168 }, { 0.148832, 0.057667 } };

    inline double PQ_EOTF(double N)
    {
        double Np = std::pow((std::max)(N, 0.0), 1.0 / 78.84375);
        double num = (std::max)(Np - 0.8359375, 0.0);
        double den = 18.8515625 - 18.6875 * Np;
        return 10000.0 * std::pow(num / (std::max)(den, 1e-10), 1.0 / 0.1593017578125);
    }

    inline double PQ_InvEOTF(double L)
    {
        double Lp = std::pow((std::max)(L, 0.0) / 10000.0, 0.1593017578125);
        double num = 0.8359375 + 18.8515625 * Lp;
        double den = 1.0 + 18.6875 * Lp;
        return std::pow(num / den, 78.84375);
    }

    inline double PQ_InvEOTF_Signed(double L)
    {
        double v = PQ_InvEOTF(std::abs(L));
        return (L < 0.0) ? -v : v;
    }

    inline V3 XYZToScRGB(V3 xyz) { return Mul(XYZ_TO_REC709, xyz); }

    // Linear Rec.709 to linear BT.2020 primaries (ITU-R BT.2087).
    inline constexpr double REC709_TO_REC2020[9] = {
        0.6274040, 0.3292820, 0.0433136,
        0.0690970, 0.9195400, 0.0113612,
        0.0163916, 0.0880132, 0.8955950 };

    // An scRGB value (1.0 = 80 nits) as an HDR10 signal: BT.2020 primaries,
    // then the PQ curve. A component still negative in BT.2020 lies outside
    // that gamut, which HDR10 cannot carry, and encodes as 0.
    inline V3 ScRGBToHdr10(V3 rgb)
    {
        const V3 bt2020 = Mul(REC709_TO_REC2020, { rgb.x * 80.0, rgb.y * 80.0, rgb.z * 80.0 });
        return { PQ_InvEOTF(bt2020.x), PQ_InvEOTF(bt2020.y), PQ_InvEOTF(bt2020.z) };
    }

    inline V3 ScRGBToICtCp(V3 rgb)
    {
        V3 xyz = Mul(REC709_TO_XYZ, rgb);
        xyz = { xyz.x * 80.0, xyz.y * 80.0, xyz.z * 80.0 };
        V3 lms = Mul(XYZ_TO_LMS_ICTCP, xyz);
        V3 pq = { PQ_InvEOTF_Signed(lms.x), PQ_InvEOTF_Signed(lms.y), PQ_InvEOTF_Signed(lms.z) };
        return Mul(PQLMS_TO_ICTCP, pq);
    }

    // Primaries for the TargetGamut / SourceGamut enum shared by the ICtCp
    // effects: 0 = sRGB, 1 = Display P3, 2 = BT.2020, 3 = Custom (the given
    // ones), 4 = DCI-P3 (adapted to D65).
    inline std::array<V2, 3> GamutPrimaries(int gamut, V2 cR, V2 cG, V2 cB)
    {
        const V2* p = GAMUT_709;
        if (gamut == 1) p = GAMUT_P3;
        else if (gamut == 2) p = GAMUT_2020;
        else if (gamut == 3) return { cR, cG, cB };
        else if (gamut == 4) p = GAMUT_DCIP3;
        return { p[0], p[1], p[2] };
    }

    // The gamut triangle's edge, sampled at `n` points (n divisible by 3) and
    // taken to ICtCp at intensity `iVal` -- exactly SampleBoundary /
    // SampleBnd in the HLSL. Writes (Ct, Cp) pairs.
    template <size_t N>
    inline void SampleBoundary(const std::array<V2, 3>& g, double iVal, std::array<V2, N>& out)
    {
        static_assert(N % 3 == 0);
        const double nits = PQ_EOTF(iVal);
        const double Ys = (std::max)(nits / 80.0, 0.0001);
        const size_t ppe = N / 3;
        for (size_t i = 0; i < N; ++i)
        {
            const size_t e = i / ppe;
            const double t = static_cast<double>(i % ppe) / static_cast<double>(ppe);
            const V2& a = g[e];
            const V2& b = g[(e + 1) % 3];
            const V2 xy{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
            const double X = (xy.y > 1e-6) ? xy.x * Ys / xy.y : 0.0;
            const double Z = (xy.y > 1e-6) ? (1.0 - xy.x - xy.y) * Ys / xy.y : 0.0;
            const V3 ic = ScRGBToICtCp(XYZToScRGB({ X, Ys, Z }));
            out[i] = { ic.y, ic.z };
        }
    }
}
