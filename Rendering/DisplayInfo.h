#pragma once

#include "pch_engine.h"

namespace ShaderLab::Rendering
{
    // Snapshot of the display's HDR / color capabilities, sourced from
    // WinRT Windows.Graphics.Display.AdvancedColorInfo (via a
    // DisplayInformation bound to the app window). Requires Windows 11
    // 22H2 (10.0.22621) — the app's declared minimum OS.
    struct DisplayCapabilities
    {
        // True when the OS reports HDR is the active advanced-color kind.
        bool hdrEnabled{ false };

        // Bits per color channel. AdvancedColorInfo does not expose scanout
        // depth, so this is derived from the active kind (WCG/HDR composite
        // in FP16 and scan out at 10-bit+ → 10; plain SDR → 8). Simulated
        // profiles (presets / ICC / MCP custom) set their own value.
        uint32_t bitsPerColor{ 8 };

        // SDR white level in nits (typically 80 for SDR; tracks the Windows
        // Settings "SDR content brightness" slider when HDR is on).
        float sdrWhiteLevelNits{ 80.0f };

        // Peak luminance the display can produce (in nits).
        float maxLuminanceNits{ 270.0f };

        // Minimum luminance the display can produce (in nits).
        float minLuminanceNits{ 0.5f };

        // Maximum full-frame luminance (in nits).
        float maxFullFrameLuminanceNits{ 270.0f };

        // Monitor color primaries (CIE xy chromaticity) from
        // AdvancedColorInfo. Default to sRGB/Rec.709 if not available.
        float redPrimaryX{ 0.64f };
        float redPrimaryY{ 0.33f };
        float greenPrimaryX{ 0.30f };
        float greenPrimaryY{ 0.60f };
        float bluePrimaryX{ 0.15f };
        float bluePrimaryY{ 0.06f };
        float whitePointX{ 0.3127f };
        float whitePointY{ 0.3290f };

        // Active color mode, mapped 1:1 from WinRT AdvancedColorKind:
        // 0 = SDR (StandardDynamicRange), 1 = WCG/ACM (WideColorGamut —
        // FP16 scRGB composition, display-referred luminance), 2 = HDR
        // (HighDynamicRange — FP16 scRGB composition, scene-referred
        // luminance).
        uint32_t activeColorMode{ 0 };

        // Capability flags. *Supported comes from
        // AdvancedColorInfo::IsAdvancedColorKindAvailable (the kind is
        // achievable on this display, whether or not it is active).
        // *UserEnabled mirrors the ACTIVE kind — AdvancedColorInfo has no
        // separate user-toggle probe, and an available-but-inactive kind
        // is exactly the "supported, not enabled" state callers care about.
        bool hdrSupported{ false };
        bool hdrUserEnabled{ false };
        bool wcgSupported{ false };
        bool wcgUserEnabled{ false };

        // Human-readable summary for the status bar.
        std::wstring ModeString() const
        {
            switch (activeColorMode)
            {
            case 2:  return L"HDR";
            case 1:  return L"WCG";
            default: return L"SDR";
            }
        }

        std::wstring LuminanceString() const
        {
            // Show both peak and SDR-reference white. SDR white drives any
            // pipeline that needs to know "what nit level does scRGB 1.0
            // resolve to" (everything in the new ICtCp suite, plus the
            // built-in tone mapper). Reading the live OS value is what
            // makes the suite track the user's "SDR content brightness"
            // slider in Windows Settings without a manual override.
            return std::format(L"{:.0f} nits (SDR white {:.0f})",
                maxLuminanceNits, sdrWhiteLevelNits);
        }
    };

    // Callback signature fired when display capabilities change.
    using DisplayChangeCallback = std::function<void(const DisplayCapabilities&)>;
}
