#pragma once

#include "pch_engine.h"

namespace ShaderLab::Rendering
{
    // Pixel format + color space of the swap chain and render targets. The
    // pipeline is always scRGB FP16 (FormatScRgbFP16); the struct exists so
    // consumers read one description instead of repeating the DXGI values.
    struct PipelineFormat
    {
        DXGI_FORMAT             dxgiFormat{ DXGI_FORMAT_R16G16B16A16_FLOAT };
        DXGI_COLOR_SPACE_TYPE   colorSpace{ DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 };
        std::wstring            name{ L"scRGB FP16" };

        // Bits per channel (for UI / inspector display).
        uint32_t bitsPerChannel{ 16 };

        // True if the format uses a linear (gamma 1.0) transfer function.
        bool isLinear{ true };

        // True if the format can represent values outside [0,1] (HDR headroom).
        bool isFloatingPoint{ true };

        bool operator==(const PipelineFormat& other) const
        {
            return dxgiFormat == other.dxgiFormat && colorSpace == other.colorSpace;
        }
    };

    // scRGB FP16: linear, full HDR range, negative values allowed. On SDR
    // displays DWM composes it through Advanced Color.
    inline const PipelineFormat FormatScRgbFP16{
        .dxgiFormat    = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .colorSpace    = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
        .name          = L"scRGB FP16",
        .bitsPerChannel = 16,
        .isLinear       = true,
        .isFloatingPoint = true,
    };
}
