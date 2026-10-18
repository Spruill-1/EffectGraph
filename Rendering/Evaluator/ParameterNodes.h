#pragma once

#include "pch_engine.h"
#include "../../Graph/EffectNode.h"

// Parameter nodes (Clock, Math Expression, Random, Working Space, plain
// parameter nodes) have no shader: their analysis fields are computed on the
// CPU from their properties. Nothing here needs a device.
namespace ShaderLab::Rendering::Evaluator
{
    // The analysis fields `node` publishes, by its NodeKind. Sets or clears
    // node.runtimeError for an expression that does not parse. `node` must
    // carry a customEffect.
    std::vector<Graph::AnalysisFieldValue> ParameterNodeFields(Graph::EffectNode& node);
}
