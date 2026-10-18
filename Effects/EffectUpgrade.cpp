#include "pch_engine.h"
#include "EffectUpgrade.h"

#include "ShaderLabEffects.h"
#include "../Graph/GraphModel.h"

namespace ShaderLab::Effects
{
    const ShaderLabEffectDescriptor* NewerDescriptorFor(const Graph::EffectNode& node)
    {
        if (!node.customEffect.has_value() || node.customEffect->shaderLabEffectId.empty())
            return nullptr;
        const auto* desc = ShaderLabEffects::Instance().FindById(node.customEffect->shaderLabEffectId);
        if (!desc || desc->effectVersion <= node.customEffect->shaderLabEffectVersion)
            return nullptr;
        return desc;
    }

    bool UpgradeNode(Graph::GraphModel& graph, Graph::EffectNode& node,
        const ShaderLabEffectDescriptor& descriptor)
    {
        auto freshNode = ShaderLabEffects::CreateNode(descriptor);
        if (!freshNode.customEffect.has_value()) return false;

        auto savedProperties = node.properties;
        node.customEffect = std::move(freshNode.customEffect.value());
        node.inputPins = std::move(freshNode.inputPins);
        graph.FitVariadicPins(node.id);   // pins are derived from the edges
        node.outputPins = std::move(freshNode.outputPins);
        node.properties = std::move(freshNode.properties);
        for (const auto& [key, value] : savedProperties)
        {
            auto it = node.properties.find(key);
            if (it != node.properties.end())
                it->second = value;
        }
        node.dirty = true;
        node.cachedOutput = nullptr;   // the evaluator's output for the old definition
        return true;
    }

    std::vector<uint32_t> UpgradeOutdatedNodes(Graph::GraphModel& graph)
    {
        std::vector<uint32_t> upgraded;
        for (auto& node : graph.MutableNodes())
        {
            const auto* desc = NewerDescriptorFor(node);
            if (desc && UpgradeNode(graph, node, *desc))
                upgraded.push_back(node.id);
        }
        return upgraded;
    }
}
