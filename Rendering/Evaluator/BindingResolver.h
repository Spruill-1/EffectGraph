#pragma once

#include "pch_engine.h"
#include "../../Graph/GraphModel.h"

// Property bindings: a node's effective properties are its authored values
// with every bound property replaced by the upstream analysis field it reads.
namespace ShaderLab::Rendering::Evaluator
{
    using PropertyMap = std::map<std::wstring, Graph::PropertyValue>;

    // Fills effectiveProps with node.properties plus resolved bindings.
    // Returns true when a bound value differs from the stored one.
    bool ResolveBindings(const Graph::EffectNode& node, const Graph::GraphModel& graph, PropertyMap& effectiveProps);

    // Writes each resolved binding back to node.properties (what the UI shows
    // and /graph/save writes). A non-finite value keeps the previous one and
    // says so on runtimeError: an analysis field can carry NaN, and a NaN
    // property makes the saved document unloadable.
    void WriteBackBindings(Graph::EffectNode& node, const PropertyMap& effectiveProps);

    // ResolveBindings, then WriteBackBindings when anything moved. Returns
    // whether anything moved; dirty handling stays with the caller.
    bool ResolveAndWriteBackBindings(Graph::EffectNode& node, const Graph::GraphModel& graph,
                                     PropertyMap& effectiveProps);
}
