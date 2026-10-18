#pragma once

#include "pch_engine.h"

namespace ShaderLab::Effects
{
    // Hint for the properties panel on which control to generate.
    enum class PropertyUIHint
    {
        Slider,         // float/int with min/max → Slider + NumberBox
        NumberBox,      // float/int without range → NumberBox only
        Checkbox,       // bool → ToggleSwitch
        ComboBox,       // uint32 with named options → ComboBox
        VectorEditor,   // float2/3/4 → row of labeled NumberBoxes
        MatrixEditor,   // D2D1_MATRIX_5X4_F → 5×4 grid of NumberBoxes
        CurveEditor,    // std::vector<float> → button to open curve editor flyout
        ReadOnly,       // Display value but don't allow editing
    };

    // Per-property metadata that drives the properties panel UI.
    struct PropertyMetadata
    {
        PropertyUIHint  uiHint{ PropertyUIHint::NumberBox };

        // Slider / NumberBox range (ignored for non-numeric types).
        float           minValue{ 0.0f };
        float           maxValue{ 1.0f };
        float           step{ 0.01f };

        // Enum labels for ComboBox (index → display name).
        // The uint32 property value is the index into this list.
        std::vector<std::wstring> enumLabels;

        // Component labels for VectorEditor (e.g. {"X","Y","Z"}).
        // Defaults applied in UI code if empty: X/Y for float2, X/Y/Z for float3, X/Y/Z/W for float4.
        std::vector<std::wstring> componentLabels;

        // Per-component min/max for vectors (parallel to componentLabels).
        // If empty, minValue/maxValue apply uniformly.
        std::vector<float> componentMin;
        std::vector<float> componentMax;
    };
}
