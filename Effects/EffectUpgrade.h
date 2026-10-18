#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Graph
{
    class GraphModel;
}

// Bringing a graph's ShaderLab effect nodes up to the registry's version.
namespace ShaderLab::Effects
{
    struct ShaderLabEffectDescriptor;

    // The registry's descriptor for `node` when it is newer than the version
    // the node was created from; nullptr otherwise.
    const ShaderLabEffectDescriptor* NewerDescriptorFor(const Graph::EffectNode& node);

    // Replaces the node's definition, pins and properties with the
    // descriptor's, keeping the values of properties that still exist, and
    // marks it dirty. The caller invalidates the evaluator's state for it.
    // Render thread only.
    bool UpgradeNode(Graph::GraphModel& graph, Graph::EffectNode& node,
        const ShaderLabEffectDescriptor& descriptor);

    // Upgrades every outdated node and returns the ids it upgraded.
    std::vector<uint32_t> UpgradeOutdatedNodes(Graph::GraphModel& graph);
}
