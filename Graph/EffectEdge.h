#pragma once

#include "pch_engine.h"

namespace ShaderLab::Graph
{
    // A directed edge in the effect graph connecting an output pin of one node
    // to an input pin of another node.
    struct EffectEdge
    {
        uint32_t sourceNodeId{ 0 };
        uint32_t sourcePin{ 0 };
        uint32_t destNodeId{ 0 };
        uint32_t destPin{ 0 };

        bool operator==(const EffectEdge& other) const = default;
    };
}
