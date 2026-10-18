#pragma once

#include <cstdint>

namespace ShaderLab::Effects
{
    // Gamut boundary table geometry: the LUT generator's hidden
    // OutputWidth/Height, and GAMUT_LUT_W/H in shaderlab_gamut.hlsli. 2048 hue
    // columns by 128 I rows, 4 MB as FP32 RGBA. Hue needs the fine step: the
    // boundary peaks sharply at each primary's hue.
    inline constexpr uint32_t cGamutLutWidth = 2048;
    inline constexpr uint32_t cGamutLutHeight = 128;
    static_assert((cGamutLutWidth & (cGamutLutWidth - 1)) == 0,
                  "GamutLutColumnFor wraps hue with a mask");
}
