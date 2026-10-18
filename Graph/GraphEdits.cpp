#include "pch_engine.h"
#include "GraphEdits.h"

namespace ShaderLab::Graph
{
    namespace
    {
        bool IsExpression(const EffectNode& node)
        {
            return node.customEffect.has_value() && node.customEffect->nodeKind == NodeKind::Expression;
        }
    }

    void SetNodeProperty(EffectNode& node, const std::wstring& key, PropertyValue value)
    {
        node.properties[key] = std::move(value);
        node.dirty = true;
    }

    bool SetNodeProperty(GraphModel& graph, uint32_t nodeId, const std::wstring& key, PropertyValue value)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        SetNodeProperty(*node, key, std::move(value));
        return true;
    }

    void SetNodeMedia(EffectNode& node, std::wstring path)
    {
        node.mediaPath = std::move(path);
        node.dirty = true;
    }

    bool SetNodeMedia(GraphModel& graph, uint32_t nodeId, std::wstring path)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        SetNodeMedia(*node, std::move(path));
        return true;
    }

    void RenameNode(EffectNode& node, std::wstring name)
    {
        node.name = std::move(name);
    }

    bool RenameNode(GraphModel& graph, uint32_t nodeId, std::wstring name)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        RenameNode(*node, std::move(name));
        return true;
    }

    void SetNodePosition(EffectNode& node, winrt::Windows::Foundation::Numerics::float2 position)
    {
        node.position = position;
    }

    void MoveNodes(GraphModel& graph, std::span<const uint32_t> nodeIds,
        winrt::Windows::Foundation::Numerics::float2 delta)
    {
        for (uint32_t id : nodeIds)
            if (auto* node = graph.FindNode(id))
                node->position += delta;
    }

    void MarkNodeChanged(EffectNode& node)
    {
        node.dirty = true;
    }

    bool MarkNodeChanged(GraphModel& graph, uint32_t nodeId)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        MarkNodeChanged(*node);
        return true;
    }

    bool RetryNode(GraphModel& graph, uint32_t nodeId)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        node->runtimeError.clear();
        node->dirty = true;
        return true;
    }

    bool ResetNodeOutput(GraphModel& graph, uint32_t nodeId)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        node->cachedOutput = nullptr;
        node->dirty = true;
        return true;
    }

    bool ReplaceCustomEffect(GraphModel& graph, uint32_t nodeId, CustomEffectDefinition definition)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        node->customEffect = std::move(definition);
        node->cachedOutput = nullptr;
        node->dirty = true;
        return true;
    }

    bool SetBindingComponent(GraphModel& graph, uint32_t nodeId, const std::wstring& key, uint32_t component)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        auto it = node->propertyBindings.find(key);
        if (it == node->propertyBindings.end()) return false;
        auto& sources = it->second.sources;
        if (!sources.empty() && sources[0].has_value())
            sources[0]->sourceComponent = component;
        node->dirty = true;
        return true;
    }

    void RecordNodeGpuTime(EffectNode& node, double milliseconds, GpuNodeState state)
    {
        node.lastGpuMs = milliseconds;
        node.gpuState = state;
    }

    EffectNode NewInstanceOf(const EffectNode& node)
    {
        EffectNode instance;
        instance.name = node.name;
        instance.type = node.type;
        instance.inputPins = node.inputPins;
        instance.outputPins = node.outputPins;
        instance.effectClsid = node.effectClsid;
        instance.properties = node.properties;
        instance.customEffect = node.customEffect;
        if (instance.customEffect.has_value())
            CoCreateGuid(&instance.customEffect->shaderGuid);
        return instance;
    }

    bool EditNodeProperty(GraphModel& graph, uint32_t nodeId, const std::wstring& key,
        const std::function<bool(PropertyValue&)>& edit)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node) return false;
        auto it = node->properties.find(key);
        if (it == node->properties.end()) return false;
        if (!edit(it->second)) return false;
        node->dirty = true;
        return true;
    }

    EffectNode CloneNodeForPaste(const EffectNode& node)
    {
        EffectNode copy = node;
        copy.dirty = true;
        copy.cachedOutput = nullptr;
        copy.runtimeError.clear();
        if (copy.customEffect.has_value())
            CoCreateGuid(&copy.customEffect->shaderGuid);
        return copy;
    }

    uint32_t ConnectClonedEdges(GraphModel& graph, std::span<const EffectEdge> edges,
        const std::unordered_map<uint32_t, uint32_t>& idMap)
    {
        uint32_t connected = 0;
        for (const auto& edge : edges)
        {
            const auto src = idMap.find(edge.sourceNodeId);
            const auto dst = idMap.find(edge.destNodeId);
            if (src == idMap.end() || dst == idMap.end()) continue;
            if (graph.Connect(src->second, edge.sourcePin, dst->second, edge.destPin))
                ++connected;
        }
        return connected;
    }

    std::vector<std::pair<uint32_t, uint32_t>> DuplicateNodes(GraphModel& graph,
        std::span<const uint32_t> nodeIds)
    {
        for (uint32_t id : nodeIds)
            if (!graph.FindNode(id)) return {};

        // FindNode pointers do not survive AddNode, so each copy is made just
        // before it is added.
        std::unordered_map<uint32_t, uint32_t> idMap;
        std::vector<std::pair<uint32_t, uint32_t>> added;
        for (uint32_t id : nodeIds)
        {
            if (idMap.contains(id)) continue;
            const uint32_t newId = graph.AddNode(CloneNodeForPaste(*graph.FindNode(id)));
            idMap[id] = newId;
            added.emplace_back(id, newId);
        }

        auto remap = [&idMap](uint32_t& sourceId)
        {
            if (const auto it = idMap.find(sourceId); it != idMap.end()) sourceId = it->second;
        };
        for (const auto& [oldId, newId] : added)
        {
            for (auto& [property, binding] : graph.FindNode(newId)->propertyBindings)
            {
                if (binding.wholeArray) remap(binding.wholeArraySourceNodeId);
                for (auto& source : binding.sources)
                    if (source.has_value()) remap(source->sourceNodeId);
            }
        }

        // Connect appends to the edge list, so it walks a copy.
        const std::vector<EffectEdge> edges = graph.Edges();
        ConnectClonedEdges(graph, edges, idMap);
        return added;
    }

    uint32_t ExpressionInputCount(const EffectNode& node)
    {
        if (!node.customEffect.has_value()) return 0;
        uint32_t count = 0;
        for (const auto& p : node.customEffect->parameters)
            if (p.typeName == L"float") ++count;
        return count;
    }

    bool CanRemoveExpressionInput(const EffectNode& node)
    {
        return IsExpression(node) && ExpressionInputCount(node) > 1;
    }

    std::wstring AddExpressionInput(GraphModel& graph, uint32_t nodeId)
    {
        auto* node = graph.FindNode(nodeId);
        if (!node || !IsExpression(*node)) return {};

        std::wstring name;
        for (wchar_t ch = L'A'; ch <= L'Z'; ++ch)
        {
            std::wstring candidate(1, ch);
            if (!node->properties.contains(candidate))
            {
                name = std::move(candidate);
                break;
            }
        }
        if (name.empty()) return {};

        ParameterDefinition param;
        param.name = name;
        param.typeName = L"float";
        param.defaultValue = 0.0f;
        param.minValue = -100000.0f;
        param.maxValue = 100000.0f;
        param.step = 0.1f;
        node->customEffect->parameters.push_back(std::move(param));
        node->properties[name] = 0.0f;
        node->dirty = true;
        return name;
    }

    bool RemoveExpressionInput(GraphModel& graph, uint32_t nodeId, const std::wstring& name)
    {
        if (name == L"Expression") return false;
        auto* node = graph.FindNode(nodeId);
        if (!node || !CanRemoveExpressionInput(*node)) return false;

        std::erase_if(node->customEffect->parameters,
            [&name](const ParameterDefinition& p) { return p.name == name; });
        node->properties.erase(name);
        graph.UnbindProperty(nodeId, name);
        node->dirty = true;
        return true;
    }
}
