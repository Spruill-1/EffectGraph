#include "pch_engine.h"
#include "ShaderLabEffects.h"

// Shared HLSL color-math library, served to shaders as
// #include "shaderlab_colormath.hlsli". The include is optional: a shader may
// carry its own copy of these functions instead.

namespace ShaderLab::Effects
{
    // -----------------------------------------------------------------------
    // Shared color math HLSL
    // -----------------------------------------------------------------------

    // The guard lets a shader include the library more than once, or include
    // it after a caller prepended it.
    static const std::string s_colorMathHLSL = R"HLSL(
#ifndef SHADERLAB_COLORMATH_HLSLI_INCLUDED
#define SHADERLAB_COLORMATH_HLSLI_INCLUDED
// ---- ShaderLab Color Math Library ----

// scRGB: linear Rec.709 primaries, 1.0 = 80 nits SDR white
// Pipeline operates in FP16 scRGB throughout.

// sRGB EOTF (decode gamma)
float3 SRGBToLinear(float3 c) {
    return float3(
        c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4),
        c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
        c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4));
}

// sRGB inverse EOTF (encode gamma)
float3 LinearToSRGB(float3 c) {
    return float3(
        c.r <= 0.0031308 ? c.r * 12.92 : 1.055 * pow(c.r, 1.0/2.4) - 0.055,
        c.g <= 0.0031308 ? c.g * 12.92 : 1.055 * pow(c.g, 1.0/2.4) - 0.055,
        c.b <= 0.0031308 ? c.b * 12.92 : 1.055 * pow(c.b, 1.0/2.4) - 0.055);
}

// scRGB (Rec.709 linear) -> CIE XYZ (D65)
static const float3x3 REC709_TO_XYZ = float3x3(
    0.4123908, 0.3575843, 0.1804808,
    0.2126390, 0.7151687, 0.0721923,
    0.0193308, 0.1191950, 0.9505322
);

// CIE XYZ (D65) -> scRGB (Rec.709 linear)
static const float3x3 XYZ_TO_REC709 = float3x3(
     3.2409699, -1.5373832, -0.4986108,
    -0.9692436,  1.8759675,  0.0415551,
     0.0556301, -0.2039770,  1.0569715
);

// scRGB -> CIE XYZ
float3 ScRGBToXYZ(float3 rgb) {
    return mul(REC709_TO_XYZ, rgb);
}

// CIE XYZ -> scRGB
float3 XYZToScRGB(float3 xyz) {
    return mul(XYZ_TO_REC709, xyz);
}

// CIE XYZ -> CIE xyY
float3 XYZToxyY(float3 xyz) {
    float sum = xyz.x + xyz.y + xyz.z;
    float2 xy = (sum < 1e-10) ? float2(0.3127, 0.3290) : float2(xyz.x / sum, xyz.y / sum);
    return float3(xy, xyz.y);
}

// CIE xyY -> CIE XYZ
float3 xyYToXYZ(float3 xyY) {
    float X = (xyY.y < 1e-10) ? 0.0 : xyY.x * xyY.z / xyY.y;
    float Z = (xyY.y < 1e-10) ? 0.0 : (1.0 - xyY.x - xyY.y) * xyY.z / xyY.y;
    return float3(X, xyY.z, Z);
}

// Luminance in nits from scRGB (1.0 scRGB = 80 nits)
float ScRGBToNits(float3 rgb) {
    return dot(rgb, float3(0.2126390, 0.7151687, 0.0721923)) * 80.0;
}

// Luminance in nits from scRGB (Y component, handles negative values)
float ScRGBLuminanceNits(float3 rgb) {
    return max(0.0, dot(rgb, float3(0.2126390, 0.7151687, 0.0721923))) * 80.0;
}

// PQ (ST.2084) EOTF: PQ signal [0,1] -> linear nits [0,10000]
float PQ_EOTF(float N) {
    float Np = pow(max(N, 0.0), 1.0 / 78.84375);
    float num = max(Np - 0.8359375, 0.0);
    float den = 18.8515625 - 18.6875 * Np;
    return 10000.0 * pow(num / max(den, 1e-10), 1.0 / 0.1593017578125);
}

// PQ inverse EOTF: linear nits [0,10000] -> PQ signal [0,1]
float PQ_InvEOTF(float L) {
    float Lp = pow(max(L, 0.0) / 10000.0, 0.1593017578125);
    float num = 0.8359375 + 18.8515625 * Lp;
    float den = 1.0 + 18.6875 * Lp;
    return pow(num / den, 78.84375);
}

// Rec.2020 linear -> CIE XYZ
static const float3x3 REC2020_TO_XYZ = float3x3(
    0.6369580, 0.1446169, 0.1688810,
    0.2627002, 0.6779981, 0.0593017,
    0.0000000, 0.0280727, 1.0609851
);

// CIE XYZ -> Rec.2020 linear
static const float3x3 XYZ_TO_REC2020 = float3x3(
     1.7166512, -0.3556708, -0.2533663,
    -0.6666844,  1.6164812,  0.0157685,
     0.0176399, -0.0427706,  0.9421031
);

// Display P3 (P3 primaries, D65 white) linear -> CIE XYZ
static const float3x3 P3D65_TO_XYZ = float3x3(
    0.4865709, 0.2656677, 0.1982173,
    0.2289746, 0.6917385, 0.0792869,
    0.0000000, 0.0451134, 1.0439444
);

// CIE XYZ -> Display P3 linear
static const float3x3 XYZ_TO_P3D65 = float3x3(
     2.4934969, -0.9313836, -0.4027108,
    -0.8294890,  1.7626641,  0.0236247,
     0.0358458, -0.0761724,  0.9568845
);

// Gamut primaries in CIE xy coordinates
// Rec.709/sRGB
static const float2 GAMUT_709_R  = float2(0.64, 0.33);
static const float2 GAMUT_709_G  = float2(0.30, 0.60);
static const float2 GAMUT_709_B  = float2(0.15, 0.06);

// P3 primaries (Display P3 with the D65 white; DCI-P3 with the DCI white)
static const float2 GAMUT_P3_R   = float2(0.680, 0.320);
static const float2 GAMUT_P3_G   = float2(0.265, 0.690);
static const float2 GAMUT_P3_B   = float2(0.150, 0.060);

// Rec.2020
static const float2 GAMUT_2020_R = float2(0.708, 0.292);
static const float2 GAMUT_2020_G = float2(0.170, 0.797);
static const float2 GAMUT_2020_B = float2(0.131, 0.046);

// D65 white point
static const float2 D65_WHITE    = float2(0.3127, 0.3290);

// DCI white point (SMPTE RP 431-2)
static const float2 DCI_WHITE    = float2(0.314, 0.351);

// DCI-P3 as the D65 working space sees it: the P3 primaries under the DCI
// white, Bradford-adapted to D65 as MakeTargetXf(4) does. Its white is D65.
static const float2 GAMUT_DCIP3_R = float2(0.680701, 0.318895);
static const float2 GAMUT_DCIP3_G = float2(0.281207, 0.674168);
static const float2 GAMUT_DCIP3_B = float2(0.148832, 0.057667);

// Check if point p is inside triangle (a, b, c) using barycentric coordinates
bool PointInTriangle(float2 p, float2 a, float2 b, float2 c) {
    float2 v0 = c - a, v1 = b - a, v2 = p - a;
    float d00 = dot(v0, v0);
    float d01 = dot(v0, v1);
    float d02 = dot(v0, v2);
    float d11 = dot(v1, v1);
    float d12 = dot(v1, v2);
    float inv = 1.0 / (d00 * d11 - d01 * d01);
    float u = (d11 * d02 - d01 * d12) * inv;
    float v = (d00 * d12 - d01 * d02) * inv;
    return (u >= 0) && (v >= 0) && (u + v <= 1.0);
}

// Turbo colormap approximation (for luminance heatmaps)
float3 TurboColormap(float t) {
    t = saturate(t);
    float r = saturate(0.13572138 + t * (4.61539260 + t * (-42.66032258 + t * (132.13108234 + t * (-152.94239396 + t * 59.28637943)))));
    float g = saturate(0.09140261 + t * (2.19418839 + t * (4.84296658 + t * (-14.18503333 + t * (4.27729857 + t * 2.82956604)))));
    float b = saturate(0.10667330 + t * (12.64194608 + t * (-60.58204836 + t * (110.36276771 + t * (-89.90310912 + t * 27.34824973)))));
    return float3(r, g, b);
}

// D65 reference white in XYZ
static const float3 D65_XYZ = float3(0.95047, 1.00000, 1.08883);

// CIE Lab helper
float LabF(float t) {
    // Signed extension: handle negative XYZ values from out-of-gamut scRGB.
    float at = abs(t);
    float ft = (at > 0.008856) ? pow(at, 1.0/3.0) : (7.787 * at + 16.0/116.0);
    return (t < 0.0) ? -ft : ft;
}

// CIE XYZ -> CIE L*a*b* (D65)
float3 XYZToLab(float3 xyz) {
    float fx = LabF(xyz.x / D65_XYZ.x);
    float fy = LabF(xyz.y / D65_XYZ.y);
    float fz = LabF(xyz.z / D65_XYZ.z);
    float L = 116.0 * fy - 16.0;
    float a = 500.0 * (fx - fy);
    float b = 200.0 * (fy - fz);
    return float3(L, a, b);
}

// scRGB -> CIE L*a*b*
float3 ScRGBToLab(float3 rgb) {
    return XYZToLab(ScRGBToXYZ(rgb));
}

// ---- ICtCp (BT.2100) ----
// Pipeline: scRGB -> XYZ -> LMS (BT.2124 cross-talk) -> PQ encode -> ICtCp

// XYZ to LMS (BT.2124 / Hunt-Pointer-Estevez with cross-talk)
static const float3x3 XYZ_TO_LMS_ICTCP = float3x3(
     0.3592832, 0.6976051, -0.0358916,
    -0.1920808, 1.1004768,  0.0753741,
     0.0070797, 0.0748262,  0.8433009
);

// LMS to XYZ (inverse)
static const float3x3 LMS_TO_XYZ_ICTCP = float3x3(
     2.0701800, -1.3264569,  0.2066510,
     0.3649882,  0.6805541, -0.0453723,
    -0.0496570, -0.0492033,  1.1880720
);

// PQ-encoded LMS to ICtCp
static const float3x3 PQLMS_TO_ICTCP = float3x3(
    2048.0/4096.0,  2048.0/4096.0,     0.0/4096.0,
    6610.0/4096.0, -13613.0/4096.0,  7003.0/4096.0,
   17933.0/4096.0, -17390.0/4096.0,  -543.0/4096.0
);

// ICtCp to PQ-encoded LMS (inverse)
static const float3x3 ICTCP_TO_PQLMS = float3x3(
    1.0,  0.008609037,  0.111029625,
    1.0, -0.008609037, -0.111029625,
    1.0,  0.560031336, -0.320627175
);

// PQ mirrored through the origin, as LabF does for Lab. scRGB carries
// wide-gamut colour as negative components, so clamping here would be an
// sRGB gamut clip; mirroring keeps the round trip lossless.
float PQ_InvEOTF_Signed(float L) {
    float v = PQ_InvEOTF(abs(L));
    return (L < 0.0) ? -v : v;
}

float PQ_EOTF_Signed(float N) {
    float v = PQ_EOTF(abs(N));
    return (N < 0.0) ? -v : v;
}

// scRGB -> ICtCp
float3 ScRGBToICtCp(float3 rgb) {
    // No clamp: negative components carry wide-gamut chroma. LMS is
    // non-negative for any real colour, so the signed PQ only engages on
    // out-of-locus or below-black input.
    float3 xyz = ScRGBToXYZ(rgb);
    // Scale to absolute nits for PQ (XYZ Y=1 = 80 nits in scRGB)
    xyz *= 80.0;
    float3 lms = mul(XYZ_TO_LMS_ICTCP, xyz);
    // PQ encode each LMS component (input in nits, output [-1,1])
    float3 pqLms = float3(
        PQ_InvEOTF_Signed(lms.x),
        PQ_InvEOTF_Signed(lms.y),
        PQ_InvEOTF_Signed(lms.z));
    return mul(PQLMS_TO_ICTCP, pqLms);
}

// True for Inf and NaN (exponent bits all ones). A bit test rather than
// isfinite(): without IEEE strictness the compiler may fold isfinite() away.
bool IsNonFinite(float v) {
    return (asuint(v) & 0x7F800000u) == 0x7F800000u;
}

// ICtCp -> scRGB
float3 ICtCpToScRGB(float3 ictcp) {
    float3 pqLms = mul(ICTCP_TO_PQLMS, ictcp);
    // Map non-finite to 0 first: clamp() of a NaN may return either bound,
    // which PQ decodes to -10000 nits. Only this direction is guarded, so the
    // ICtCp Round-Trip Validator still surfaces non-finite input.
    pqLms = float3(
        IsNonFinite(pqLms.x) ? 0.0 : pqLms.x,
        IsNonFinite(pqLms.y) ? 0.0 : pqLms.y,
        IsNonFinite(pqLms.z) ? 0.0 : pqLms.z);
    // PQ_EOTF goes singular past |V| = 1; clamp the magnitude, keep the sign.
    pqLms = clamp(pqLms, -1.0, 1.0);
    // PQ decode to nits
    float3 lms = float3(
        PQ_EOTF_Signed(pqLms.x),
        PQ_EOTF_Signed(pqLms.y),
        PQ_EOTF_Signed(pqLms.z));
    float3 xyz = mul(LMS_TO_XYZ_ICTCP, lms);
    // Scale back from nits to scRGB (80 nits = 1.0)
    xyz /= 80.0;
    return XYZToScRGB(xyz);
}

// ---- Delta E ITP (ITU-R BT.2124) ----
// The HDR/WCG colour-difference metric; CIE Lab metrics lose meaning above
// SDR and outside sRGB.
//   dE_ITP = 720 * sqrt(dI^2 + dT^2 + dP^2),  T = 0.5 * Ct,  P = Cp
// One unit is about one JND. Takes PQ-encoded ICtCp triples, not scRGB.
float DeltaEITP(float3 ictcp1, float3 ictcp2) {
    float dI = ictcp1.x - ictcp2.x;
    float dT = 0.5 * (ictcp1.y - ictcp2.y);
    float dP = ictcp1.z - ictcp2.z;
    return 720.0 * sqrt(dI * dI + dT * dT + dP * dP);
}

// Convenience: dE ITP straight from two scRGB colours.
float DeltaEITPFromScRGB(float3 rgb1, float3 rgb2) {
    return DeltaEITP(ScRGBToICtCp(rgb1), ScRGBToICtCp(rgb2));
}

// ---- CIE Lab colour differences ----
// For SDR, sRGB-gamut comparisons; prefer DeltaEITP for HDR or wide gamut.
// Named Cie* because graphs saved before these moved here define DeltaE76,
// DeltaE94 and DeltaE2000 themselves.

// CIE76: simple Euclidean distance in L*a*b*
float CieDeltaE76(float3 lab1, float3 lab2) {
    float3 d = lab1 - lab2;
    return sqrt(dot(d, d));
}

// CIE94 (graphic arts)
float CieDeltaE94(float3 lab1, float3 lab2) {
    float dL = lab1.x - lab2.x;
    float C1 = sqrt(lab1.y * lab1.y + lab1.z * lab1.z);
    float C2 = sqrt(lab2.y * lab2.y + lab2.z * lab2.z);
    float dC = C1 - C2;
    float da = lab1.y - lab2.y;
    float db = lab1.z - lab2.z;
    float dH2 = da * da + db * db - dC * dC;
    float dH = sqrt(max(dH2, 0.0));
    float SL = 1.0;
    float SC = 1.0 + 0.045 * C1;
    float SH = 1.0 + 0.015 * C1;
    float t1 = dL / SL;
    float t2 = dC / SC;
    float t3 = dH / SH;
    return sqrt(t1*t1 + t2*t2 + t3*t3);
}

// CIEDE2000
float CieDeltaE2000(float3 lab1, float3 lab2) {
    float L1 = lab1.x, a1 = lab1.y, b1 = lab1.z;
    float L2 = lab2.x, a2 = lab2.y, b2 = lab2.z;

    float Cab1 = sqrt(a1*a1 + b1*b1);
    float Cab2 = sqrt(a2*a2 + b2*b2);
    float Cab_avg = (Cab1 + Cab2) * 0.5;
    float Cab_avg7 = pow(Cab_avg, 7.0);
    float G = 0.5 * (1.0 - sqrt(Cab_avg7 / (Cab_avg7 + 6103515625.0))); // 25^7

    float a1p = a1 * (1.0 + G);
    float a2p = a2 * (1.0 + G);
    float C1p = sqrt(a1p*a1p + b1*b1);
    float C2p = sqrt(a2p*a2p + b2*b2);

    // Hue angles. atan2(0, 0) is implementation-defined and on some
    // drivers returns NaN -- guard against (a,b)==(0,0) by treating C==0
    // as h==0 (the hue angle of an achromatic point is meaningless and
    // any consistent value works because the C-weighted hp_avg branch
    // below short-circuits to h1p+h2p).
    float h1p = (C1p < 1e-10) ? 0.0 : atan2(b1, a1p);
    if (h1p < 0.0) h1p += 6.28318530718;
    float h2p = (C2p < 1e-10) ? 0.0 : atan2(b2, a2p);
    if (h2p < 0.0) h2p += 6.28318530718;

    float dLp = L2 - L1;
    float dCp = C2p - C1p;

    float dhp;
    if (C1p * C2p < 1e-10) dhp = 0.0;
    else {
        dhp = h2p - h1p;
        if (dhp > 3.14159265359) dhp -= 6.28318530718;
        else if (dhp < -3.14159265359) dhp += 6.28318530718;
    }
    float dHp = 2.0 * sqrt(C1p * C2p) * sin(dhp * 0.5);

    float Lp_avg = (L1 + L2) * 0.5;
    float Cp_avg = (C1p + C2p) * 0.5;

    float hp_avg;
    if (C1p * C2p < 1e-10) hp_avg = h1p + h2p;
    else {
        hp_avg = (h1p + h2p) * 0.5;
        if (abs(h1p - h2p) > 3.14159265359) hp_avg += 3.14159265359;
    }

    float T = 1.0
        - 0.17 * cos(hp_avg - 0.52359877559)       // 30 deg
        + 0.24 * cos(2.0 * hp_avg)
        + 0.32 * cos(3.0 * hp_avg + 0.10471975512)  // 6 deg
        - 0.20 * cos(4.0 * hp_avg - 1.09955742876);  // 63 deg

    float Lp50sq = (Lp_avg - 50.0) * (Lp_avg - 50.0);
    float SL = 1.0 + 0.015 * Lp50sq / sqrt(20.0 + Lp50sq);
    float SC = 1.0 + 0.045 * Cp_avg;
    float SH = 1.0 + 0.015 * Cp_avg * T;

    float Cp_avg7 = pow(Cp_avg, 7.0);
    float RC = 2.0 * sqrt(Cp_avg7 / (Cp_avg7 + 6103515625.0));
    float hp_deg = hp_avg * 57.29577951; // rad to deg
    float dtheta = 30.0 * exp(-((hp_deg - 275.0) / 25.0) * ((hp_deg - 275.0) / 25.0));
    float RT = -sin(2.0 * dtheta * 0.01745329252) * RC;

    float t1 = dLp / SL;
    float t2 = dCp / SC;
    float t3 = dHp / SH;
    return sqrt(t1*t1 + t2*t2 + t3*t3 + RT * t2 * t3);
}

// OKLab: linear sRGB -> OKLab
float3 LinearToOKLab(float3 c) {
    float l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    float m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    float s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    float l_ = pow(max(l, 0.0), 1.0/3.0);
    float m_ = pow(max(m, 0.0), 1.0/3.0);
    float s_ = pow(max(s, 0.0), 1.0/3.0);
    return float3(
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
    );
}

// ---- I-channel (PQ-encoded nits) helpers for ICtCp tone mapping ----
// In BT.2100 ICtCp, I is a weighted PQ-encoded sum of LMS. For
// chromaticity-preserving operations (compress / expand only I, leave
// Ct/Cp), it is standard to treat I as if it were PQ(neutral_nits) and
// design the curve in nits-via-PQ space. This is what BT.2390 does.

// Convert a nit value to its corresponding I coordinate.
float NitsToI(float nits) {
    return PQ_InvEOTF(max(nits, 0.0));
}

// Convert an I coordinate back to nits.
float IToNits(float I) {
    return PQ_EOTF(I);
}

// Reinhard compression on I, expressed in I-space directly. Anchored
// Mobius: maps 0 -> 0 and peakIn_I -> peakOut_I exactly, with f'(0)=1
// (linear at the low end) and smooth rolloff near peakIn. Both peaks
// are I coordinates (PQ values). For HDR -> SDR pass peakIn = HDR_I,
// peakOut = SDR_I. Inputs above peakIn_I are clamped so the curve
// can't walk past its anchor onto the rising branch beyond peakIn.
float ReinhardCompressI(float I, float peakIn_I, float peakOut_I) {
    float pp = peakIn_I * peakOut_I;
    if (pp <= 1e-12) return 0.0;
    float Ic = clamp(I, 0.0, peakIn_I);
    float denom = pp + Ic * (peakIn_I - peakOut_I);
    return Ic * pp / max(denom, 1e-12);
}

// Inverse of ReinhardCompressI: given an I value in [0, peakOut_I]
// returns the I in [0, peakIn_I] that would compress to it. Same
// peakIn/peakOut convention as ReinhardCompressI: peakIn is the
// *uncompressed* range, peakOut is the *compressed* range. For
// SDR -> HDR expansion callers pass peakIn = HDR_I, peakOut = SDR_I.
// Inputs above peakOut_I are clamped so the curve saturates at peakIn
// rather than racing toward the asymptote.
float ReinhardExpandI(float I, float peakIn_I, float peakOut_I) {
    float pp = peakIn_I * peakOut_I;
    if (pp <= 1e-12) return 0.0;
    float Ic = clamp(I, 0.0, peakOut_I);
    float denom = pp - Ic * (peakIn_I - peakOut_I);
    return Ic * pp / max(denom, 1e-12);
}

// ---- 8-bit display-referred output helpers -----------------------------
// For effects that hand back 8bpc and own the quantizer. The above-white band
// survives the tone curve in few codes, so quantizing without dither bands.

// Interleaved Gradient Noise (Jimenez 2014). Cheap and deterministic, with
// better spectral behaviour than hash-based white noise.
// Expects integer pixel coordinates; returns [0, 1).
float InterleavedGradientNoise(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Symmetric zero-mean dither noise for DitherQuantize, nominal [-1, 1] LSB.
// Not a TPDF: the two IGN samples are rotations of each other, so the density
// is stepped, with support about +-0.62. That is enough to keep rounding
// mean-preserving, which is all the quantizer needs.
float RotatedIgnDither(float2 p) {
    float n1 = InterleavedGradientNoise(p);
    float n2 = InterleavedGradientNoise(p + 5.588238);
    return n1 + n2 - 1.0;
}

// Quantize an already-encoded [0,1] value to `levels` steps with dither.
// `strength` scales the dither amplitude: 1.0 = full, 0 = none. Anything
// below full amplitude leaves the rounding biased, so do not scale it down.
// floor() the position before hashing: D2D's SCENE_POSITION is not exact and
// its error moves when the effect is re-tiled.
float3 DitherQuantize(float3 encoded, float2 p, float levels, float strength) {
    float maxCode = max(levels - 1.0, 1.0);
    float3 dither = RotatedIgnDither(floor(p)) * strength;
    return saturate(round(saturate(encoded) * maxCode + dither) / maxCode);
}

// Soft gamut-distance compression (1D). `d` is a chroma radius normalized so
// the gamut boundary is 1.0. Returns the remapped radius:
//   d <= threshold -> unchanged; d == limit -> exactly 1.0; monotone and C1
//   at the threshold; d > limit may land slightly past 1.0 (ACES-style).
// power is the knee hardness: 1 is Reinhard, higher tracks identity longer.
// ACES RGC ships 1.2.
float SoftCompressDistance(float d, float threshold, float limit, float power) {
    float t = clamp(threshold, 0.0, 0.99);
    float l = max(limit, 1.01);
    float p = clamp(power, 1.0, 8.0);
    if (d <= t) return d;
    // ACES-RGC-style power curve y = t + x / (1 + (x/s)^p)^(1/p), with
    // the scale s solved from the anchor f(l - t) == 1 - t, so d == l
    // lands exactly on the boundary. f'(0) == 1 keeps the join C1.
    float x = d - t;
    float s = (l - t) / pow(pow((l - t) / (1.0 - t), p) - 1.0, 1.0 / p);
    return t + x / pow(1.0 + pow(x / s, p), 1.0 / p);
}

// ---- Target gamut (shared with the Gamut Boundary LUT and Gamut Map) ----
// The target is an RGB cube given by xy primaries and an xy white point. The
// colour math stays in D65 (ICtCp is defined there); only the in-target test
// and the final encode use the target's RGB, through one 3x3 matrix.
// A non-D65 white is reached by Bradford adaptation folded into that matrix
// (relative colorimetric), so a D65 neutral stays neutral on the target.
//
// TargetGamut: 0 = sRGB / Rec.709, 1 = Display P3 (D65), 2 = BT.2020,
//              3 = Custom (primaries + white point from parameters),
//              4 = DCI-P3 (P3 primaries, DCI white, adapted to D65).
struct TargetXf
{
    float3x3 fromScRGB;   // scRGB (linear 709, D65) -> linear target RGB
    float3x3 toScRGB;     // its inverse
    float3   lumaT;       // luminance weights applied to linear TARGET RGB
    float    fingerprint; // identifies the target; stamped into LUT alpha
    bool     identity;    // sRGB: every transform is skipped, bit for bit
};

// Adjugate inverse. Rows r0..r2; the cofactor columns are the pairwise cross
// products, and det = r0 . (r1 x r2).
float3x3 Inverse3x3(float3x3 m)
{
    float3 c0 = cross(m[1], m[2]);
    float3 c1 = cross(m[2], m[0]);
    float3 c2 = cross(m[0], m[1]);
    float  det = dot(m[0], c0);
    return transpose(float3x3(c0, c1, c2)) / det;
}

float3 XyToXYZ(float2 xy)   // Y = 1
{
    return float3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y);
}

// Standard normalised primary matrix: linear RGB -> XYZ with (1,1,1) -> white.
// Returns false in `ok` for degenerate primaries (collinear, or y = 0).
float3x3 RgbToXyzFromPrimaries(float2 r, float2 g, float2 b, float2 w, out bool ok)
{
    ok = (r.y > 1e-6) && (g.y > 1e-6) && (b.y > 1e-6) && (w.y > 1e-6);
    float3x3 P = transpose(float3x3(XyToXYZ(ok ? r : float2(0.64, 0.33)),
                                    XyToXYZ(ok ? g : float2(0.30, 0.60)),
                                    XyToXYZ(ok ? b : float2(0.15, 0.06))));
    float det = dot(P[0], cross(P[1], P[2]));
    ok = ok && abs(det) > 1e-6;
    float3 S = mul(Inverse3x3(ok ? P : float3x3(1,0,0, 0,1,0, 0,0,1)),
                   XyToXYZ(ok ? w : float2(0.3127, 0.3290)));
    return float3x3(P[0] * S, P[1] * S, P[2] * S);   // P * diag(S)
}

// Bradford cone-response matrix and its inverse (Lam 1985 / ICC).
static const float3x3 BRADFORD = float3x3(
     0.8951,  0.2664, -0.1614,
    -0.7502,  1.7135,  0.0367,
     0.0389, -0.0685,  1.0296);
static const float3x3 BRADFORD_INV = float3x3(
     0.9869929, -0.1470543,  0.1599627,
     0.4323053,  0.5183603,  0.0492912,
    -0.0085287,  0.0400428,  0.9684867);

// The xy primaries of gamut id 0 sRGB, 1 Display P3, 2 BT.2020, 3 custom
// (cR, cG, cB), 4 DCI-P3 adapted to D65; any other id is sRGB. Ternaries, not
// if/else: every operand is evaluated, so custom primaries read from a cbuffer
// stay referenced on all paths and are never stripped.
void GamutPrimariesFor(uint gamut, float2 cR, float2 cG, float2 cB,
                       out float2 r, out float2 g, out float2 b)
{
    r = (gamut == 1) ? GAMUT_P3_R : (gamut == 2) ? GAMUT_2020_R : (gamut == 3) ? cR : (gamut == 4) ? GAMUT_DCIP3_R : GAMUT_709_R;
    g = (gamut == 1) ? GAMUT_P3_G : (gamut == 2) ? GAMUT_2020_G : (gamut == 3) ? cG : (gamut == 4) ? GAMUT_DCIP3_G : GAMUT_709_G;
    b = (gamut == 1) ? GAMUT_P3_B : (gamut == 2) ? GAMUT_2020_B : (gamut == 3) ? cB : (gamut == 4) ? GAMUT_DCIP3_B : GAMUT_709_B;
}

TargetXf MakeTargetXf(uint gamut, float2 cR, float2 cG, float2 cB, float2 cW)
{
    TargetXf t;
    // Ternaries, not if/else: HLSL evaluates every operand, so the custom
    // cbuffer members stay referenced on all paths and are never stripped.
    bool p3 = (gamut == 1) || (gamut == 4);
    float2 r = p3 ? GAMUT_P3_R : (gamut == 2) ? GAMUT_2020_R : cR;
    float2 g = p3 ? GAMUT_P3_G : (gamut == 2) ? GAMUT_2020_G : cG;
    float2 b = p3 ? GAMUT_P3_B : (gamut == 2) ? GAMUT_2020_B : cB;
    float2 w = (gamut == 3) ? cW : (gamut == 4) ? DCI_WHITE : D65_WHITE;

    bool ok;
    float3x3 npmT = RgbToXyzFromPrimaries(r, g, b, w, ok);

    // CAT(D65 -> w) = B^-1 diag(Bw / Bd65) B, and its inverse swaps the ratio.
    float3 coneD = mul(BRADFORD, XyToXYZ(D65_WHITE));
    float3 coneW = mul(BRADFORD, XyToXYZ(w));
    float3 k  = coneW / coneD;
    float3x3 cat    = mul(BRADFORD_INV, float3x3(BRADFORD[0] * k.x,
                                                 BRADFORD[1] * k.y,
                                                 BRADFORD[2] * k.z));
    float3x3 catInv = mul(BRADFORD_INV, float3x3(BRADFORD[0] / k.x,
                                                 BRADFORD[1] / k.y,
                                                 BRADFORD[2] / k.z));

    t.identity = (gamut == 0) || !ok;   // degenerate custom input -> sRGB
    // scRGB -> XYZ(D65) -> adapted XYZ(w) -> target RGB, and back.
    float3x3 fwd = mul(Inverse3x3(npmT), mul(cat, REC709_TO_XYZ));
    float3x3 inv = mul(XYZ_TO_REC709, mul(catInv, npmT));
    const float3x3 I3 = float3x3(1,0,0, 0,1,0, 0,0,1);
    t.fromScRGB = t.identity ? I3 : fwd;
    t.toScRGB   = t.identity ? I3 : inv;
    // Luminance of a TARGET-RGB triple, in the working space: Y = w709 . rgb709
    // = (w709 * toScRGB) . rgbT. For sRGB this is exactly the Rec.709 weights.
    t.lumaT = t.identity ? float3(0.2126, 0.7152, 0.0722)
                         : mul(float3(0.2126, 0.7152, 0.0722), t.toScRGB);
    // A position-weighted sum of the matrix, exactly 15 for the identity.
    // Different gamuts land close together here, so readers compare it
    // tightly. Always > 0.5, so it doubles as the "a table is wired" flag.
    t.fingerprint = dot(t.fromScRGB[0], float3(1, 2, 3))
                  + dot(t.fromScRGB[1], float3(4, 5, 6))
                  + dot(t.fromScRGB[2], float3(7, 8, 9));
    return t;
}

float3 ScRGBToTarget(TargetXf t, float3 rgb) { return t.identity ? rgb : mul(t.fromScRGB, rgb); }
float3 TargetToScRGB(TargetXf t, float3 rgb) { return t.identity ? rgb : mul(t.toScRGB, rgb); }

// Luminance with negative components ignored, measured in the TARGET's RGB:
// a colour inside the target has no negative target components, so the clip
// only discards genuine out-of-target residue. (Clipping in scRGB instead would
// under-read every legal P3 colour, which is negative in Rec.709 by nature.)
float TargetLuma(TargetXf t, float3 scRGB)
{
    return dot(max(ScRGBToTarget(t, scRGB), 0.0), t.lumaT);
}

#endif
)HLSL";

    const std::string& GetColorMathHLSL()
    {
        return s_colorMathHLSL;
    }

}
