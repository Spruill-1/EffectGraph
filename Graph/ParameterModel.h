#pragma once

#include "pch_engine.h"
#include "EffectNode.h"

// Parameter-node semantics (Float Parameter and its kin: a node whose Value
// property is the number it outputs), shared by the canvas slider and any
// other client that sets a parameter by position.
namespace ShaderLab::Graph
{
    // A parameter node's range: its Min / Max properties when it has them,
    // else the declared range of its Value parameter, and that parameter's step.
    struct ParameterRange
    {
        float min{ 0.0f };
        float max{ 1.0f };
        float step{ 0.01f };
    };
    ParameterRange ReadParameterRange(const EffectNode& node);

    // Where Value sits in its range, from 0 to 1.
    float ParameterNormalizedValue(const EffectNode& node);

    // Sets Value to the point `normalized` (0 to 1, clamped) of the range,
    // snapped to the step and marks the node dirty. Returns false, writing
    // nothing, when the node has no Value or the value would not move.
    bool SetParameterNormalized(EffectNode& node, double normalized);
}
