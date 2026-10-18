#pragma once

#include "pch_engine.h"
#include "EffectNode.h"

// Display text for property values and analysis fields, shared by the
// Properties panel, the pixel trace and the node log.
namespace ShaderLab::Graph
{
    // A scalar field: its components to 4 decimals, separated by two spaces.
    // An array field: "[N elements]", followed, when maxElements > 0, by a
    // newline and up to maxElements "  [i] values" lines and a
    // "  ... (K more)" line for the rest, each ending in a newline.
    std::wstring FormatAnalysisField(const AnalysisFieldValue& field, uint32_t maxElements);

    // Floats and vector components to `decimals` places (vectors joined by
    // ", "), integers in full, bool as true/false, strings as they are, a
    // curve as "[N floats]" and a matrix as "(matrix)".
    std::wstring FormatPropertyValue(const PropertyValue& value, int decimals);

    // The enum label `value` selects (rounded to the nearest index) when the
    // node declares `parameter` with labels; nothing otherwise.
    std::optional<std::wstring> EnumLabelFor(const EffectNode& node,
                                                           std::wstring_view parameter, float value);
}
