#pragma once

#include "pch_engine.h"
#include "DisplayInfo.h"

namespace ShaderLab::Rendering
{
    // CIE xy chromaticity coordinate.
    struct ChromaticityXY
    {
        float x{ 0.0f };
        float y{ 0.0f };
    };

    // Identifies a well-known display color gamut.
    enum class GamutId : uint32_t
    {
        sRGB = 0,
        DCI_P3,
        BT2020,
        Custom,
    };

    inline std::wstring GamutIdToString(GamutId id)
    {
        switch (id)
        {
        case GamutId::sRGB:    return L"sRGB";
        case GamutId::DCI_P3:  return L"DCI-P3";
        case GamutId::BT2020:  return L"BT.2020";
        case GamutId::Custom:  return L"Custom";
        default:               return L"Unknown";
        }
    }

    // Classifies a set of CIE xy primaries against the well-known gamuts.
    // Returns Custom when the primaries don't match any standard within
    // tolerance — real panels frequently land there, so Custom is a normal
    // result rather than an error. Shared by the ICC path and the live-display
    // path so both classify identically.
    inline GamutId DetectGamut(const ChromaticityXY& r,
                               const ChromaticityXY& g,
                               const ChromaticityXY& b) noexcept
    {
        auto close = [](float v, float target, float tol = 0.02f)
        {
            return std::abs(v - target) < tol;
        };

        // sRGB / BT.709
        if (close(r.x, 0.64f) && close(r.y, 0.33f) &&
            close(g.x, 0.30f) && close(g.y, 0.60f) &&
            close(b.x, 0.15f) && close(b.y, 0.06f))
            return GamutId::sRGB;

        // DCI-P3 / Display P3
        if (close(r.x, 0.680f) && close(r.y, 0.320f) &&
            close(g.x, 0.265f) && close(g.y, 0.690f) &&
            close(b.x, 0.150f) && close(b.y, 0.060f))
            return GamutId::DCI_P3;

        // BT.2020
        if (close(r.x, 0.708f) && close(r.y, 0.292f) &&
            close(g.x, 0.170f) && close(g.y, 0.797f) &&
            close(b.x, 0.131f) && close(b.y, 0.046f))
            return GamutId::BT2020;

        return GamutId::Custom;
    }

    // Extended display profile combining capabilities with colorimetry.
    struct DisplayProfile
    {
        DisplayCapabilities caps;

        // Colorimetry — sRGB defaults
        ChromaticityXY primaryRed{ 0.64f, 0.33f };
        ChromaticityXY primaryGreen{ 0.30f, 0.60f };
        ChromaticityXY primaryBlue{ 0.15f, 0.06f };
        ChromaticityXY whitePoint{ 0.3127f, 0.3290f };  // D65
        GamutId gamut{ GamutId::sRGB };

        // Profile metadata
        std::wstring profileName;
        bool isSimulated{ false };
    };

    // The live display as a profile: the caps, and the primaries and gamut
    // they report.
    inline DisplayProfile ProfileFromCaps(const DisplayCapabilities& caps)
    {
        DisplayProfile p{};
        p.caps = caps;
        p.isSimulated = false;
        p.profileName = L"Live Display";
        p.primaryRed   = { caps.redPrimaryX,   caps.redPrimaryY };
        p.primaryGreen = { caps.greenPrimaryX, caps.greenPrimaryY };
        p.primaryBlue  = { caps.bluePrimaryX,  caps.bluePrimaryY };
        p.whitePoint   = { caps.whitePointX,   caps.whitePointY };
        // Classified from the panel's primaries; the struct default (sRGB)
        // would misreport every wide-gamut panel.
        p.gamut = DetectGamut(p.primaryRed, p.primaryGreen, p.primaryBlue);
        return p;
    }

    // Copies a profile's primaries into its caps, so code reading only the
    // caps (CachedCapabilities, the display-change callback) sees the same
    // colorimetry as code reading the profile.
    inline void SyncCapsPrimaries(DisplayProfile& p)
    {
        p.caps.redPrimaryX   = p.primaryRed.x;   p.caps.redPrimaryY   = p.primaryRed.y;
        p.caps.greenPrimaryX = p.primaryGreen.x; p.caps.greenPrimaryY = p.primaryGreen.y;
        p.caps.bluePrimaryX  = p.primaryBlue.x;  p.caps.bluePrimaryY  = p.primaryBlue.y;
        p.caps.whitePointX   = p.whitePoint.x;   p.caps.whitePointY   = p.whitePoint.y;
    }

    // -----------------------------------------------------------------------
    // Preset factory functions
    // -----------------------------------------------------------------------

    // Preset helper: stamp coherent ACM/WCG/activeColorMode flags and the
    // derived bits-per-channel into a profile from its hdrEnabled + gamut.
    // Call AFTER caps.hdrEnabled and p.gamut are set. Used by every
    // Preset*() factory, the ICC path, and the MCP custom-profile route so
    // simulated profiles report a self-consistent display mode through the
    // Working Space node and other consumers. Mirrors the live-display
    // derivation in DisplayMonitor's CapsFromAdvancedColorInfo: a
    // wide-gamut SDR profile simulates Windows ACM (activeColorMode 1).
    inline void StampSimulatedColorMode(DisplayProfile& p)
    {
        const bool hdr  = p.caps.hdrEnabled;
        const bool wide = (p.gamut != GamutId::sRGB);
        p.caps.activeColorMode = hdr ? 2u : (wide ? 1u : 0u); // 2=HDR, 1=WCG/ACM, 0=SDR
        p.caps.hdrSupported    = hdr;
        p.caps.hdrUserEnabled  = hdr;
        p.caps.wcgSupported    = hdr || wide;
        p.caps.wcgUserEnabled  = !hdr && wide;
        p.caps.bitsPerColor    = (p.caps.activeColorMode != 0u) ? 10u : 8u;
    }

    inline DisplayProfile PresetSrgbSdr()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = false;
        p.caps.sdrWhiteLevelNits = 80.0f;
        p.caps.maxLuminanceNits = 80.0f;
        p.caps.minLuminanceNits = 0.5f;
        p.caps.maxFullFrameLuminanceNits = 80.0f;
        p.gamut = GamutId::sRGB;
        StampSimulatedColorMode(p);
        p.profileName = L"sRGB SDR (80 nits)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetSrgb270()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = false;
        p.caps.sdrWhiteLevelNits = 100.0f;
        p.caps.maxLuminanceNits = 270.0f;
        p.caps.minLuminanceNits = 0.5f;
        p.caps.maxFullFrameLuminanceNits = 270.0f;
        p.gamut = GamutId::sRGB;
        StampSimulatedColorMode(p);
        p.profileName = L"sRGB SDR (270 nits, typical laptop)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetP3_600()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.sdrWhiteLevelNits = 203.0f;
        p.caps.maxLuminanceNits = 600.0f;
        p.caps.minLuminanceNits = 0.05f;
        p.caps.maxFullFrameLuminanceNits = 500.0f;
        // DCI-P3 primaries
        p.primaryRed   = { 0.680f, 0.320f };
        p.primaryGreen = { 0.265f, 0.690f };
        p.primaryBlue  = { 0.150f, 0.060f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::DCI_P3;
        StampSimulatedColorMode(p);
        p.profileName = L"DCI-P3 HDR (600 nits, MacBook Pro-class)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetP3_1000()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.sdrWhiteLevelNits = 203.0f;
        p.caps.maxLuminanceNits = 1000.0f;
        p.caps.minLuminanceNits = 0.05f;
        p.caps.maxFullFrameLuminanceNits = 600.0f;
        // DCI-P3 primaries
        p.primaryRed   = { 0.680f, 0.320f };
        p.primaryGreen = { 0.265f, 0.690f };
        p.primaryBlue  = { 0.150f, 0.060f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::DCI_P3;
        StampSimulatedColorMode(p);
        p.profileName = L"DCI-P3 HDR (1000 nits, reference monitor)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetBT2020_1000()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.sdrWhiteLevelNits = 203.0f;
        p.caps.maxLuminanceNits = 1000.0f;
        p.caps.minLuminanceNits = 0.005f;
        p.caps.maxFullFrameLuminanceNits = 600.0f;
        // BT.2020 primaries
        p.primaryRed   = { 0.708f, 0.292f };
        p.primaryGreen = { 0.170f, 0.797f };
        p.primaryBlue  = { 0.131f, 0.046f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::BT2020;
        StampSimulatedColorMode(p);
        p.profileName = L"BT.2020 HDR (1000 nits, HDR TV)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetBT2020_4000()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = true;
        p.caps.sdrWhiteLevelNits = 203.0f;
        p.caps.maxLuminanceNits = 4000.0f;
        p.caps.minLuminanceNits = 0.005f;
        p.caps.maxFullFrameLuminanceNits = 1000.0f;
        // BT.2020 primaries
        p.primaryRed   = { 0.708f, 0.292f };
        p.primaryGreen = { 0.170f, 0.797f };
        p.primaryBlue  = { 0.131f, 0.046f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::BT2020;
        StampSimulatedColorMode(p);
        p.profileName = L"BT.2020 HDR (4000 nits, mastering)";
        p.isSimulated = true;
        return p;
    }

    inline DisplayProfile PresetAdobeRGB()
    {
        DisplayProfile p{};
        p.caps.hdrEnabled = false;
        p.caps.sdrWhiteLevelNits = 160.0f;
        p.caps.maxLuminanceNits = 160.0f;
        p.caps.minLuminanceNits = 0.5f;
        p.caps.maxFullFrameLuminanceNits = 160.0f;
        // Adobe RGB (1998) primaries
        p.primaryRed   = { 0.6400f, 0.3300f };
        p.primaryGreen = { 0.2100f, 0.7100f };
        p.primaryBlue  = { 0.1500f, 0.0600f };
        p.whitePoint   = { 0.3127f, 0.3290f };
        p.gamut = GamutId::Custom;
        StampSimulatedColorMode(p);
        p.profileName = L"Adobe RGB (1998)";
        p.isSimulated = true;
        return p;
    }

    inline std::vector<DisplayProfile> AllPresets()
    {
        return {
            PresetSrgbSdr(),
            PresetSrgb270(),
            PresetAdobeRGB(),
            PresetP3_600(),
            PresetP3_1000(),
            PresetBT2020_1000(),
            PresetBT2020_4000(),
        };
    }
}
