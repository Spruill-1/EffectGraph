#include "pch_engine.h"
#include "GraphModel.h"
#include "../Engine/Api/EffectGraphVersion.h"

#include <winrt/Windows.Data.Json.h>

namespace WDJ = winrt::Windows::Data::Json;

namespace ShaderLab::Graph
{
    // -----------------------------------------------------------------------
    // Node management
    // -----------------------------------------------------------------------
     
    uint32_t GraphModel::AddNode(EffectNode node)
    {
        node.id = m_nextId++;
        node.dirty = true;
        m_nodes.push_back(std::move(node));
        return m_nodes.back().id;
    }

    void GraphModel::RemoveNode(uint32_t nodeId)
    {
        // Any node may go, the last Output included: an output-less graph
        // is valid, and evaluation no-ops until an Output is added.

        // Everything that consumed the node -- by edge or by binding -- loses
        // an input, so it is dirty. This is what lets callers dirty only what
        // changed instead of the whole graph.
        for (const auto& e : m_edges)
            if (e.sourceNodeId == nodeId)
                if (auto* dst = FindNode(e.destNodeId)) dst->dirty = true;

        // Remove all edges referencing this node, then refit the pins of the
        // nodes it fed.
        std::vector<uint32_t> consumerIds;
        for (const auto& e : m_edges)
            if (e.sourceNodeId == nodeId) consumerIds.push_back(e.destNodeId);
        std::erase_if(m_edges, [nodeId](const EffectEdge& e)
        {
            return e.sourceNodeId == nodeId || e.destNodeId == nodeId;
        });
        for (uint32_t consumerId : consumerIds) FitVariadicPins(consumerId);

        // Remove property bindings referencing this node as a source.
        for (auto& node : m_nodes)
        {
            const size_t removed = std::erase_if(node.propertyBindings, [nodeId](const auto& pair)
            {
                return pair.second.ReadsFrom(nodeId);
            });
            if (removed) node.dirty = true;
        }

        // Remove the node itself.
        std::erase_if(m_nodes, [nodeId](const EffectNode& n)
        {
            return n.id == nodeId;
        });
    }

    bool GraphModel::EnsureUniqueCustomEffectName(uint32_t nodeId)
    {
        auto* node = FindNode(nodeId);
        if (!node || !node->customEffect.has_value()) return false;
        const std::wstring& hlsl = node->customEffect->hlslSource;

        // Taken: another custom effect node has the name with different HLSL.
        auto taken = [&](const std::wstring& name)
        {
            return std::ranges::any_of(m_nodes, [&](const EffectNode& other)
            {
                return other.id != nodeId && other.name == name &&
                       other.customEffect.has_value() && other.customEffect->hlslSource != hlsl;
            });
        };
        if (!taken(node->name)) return false;

        std::wstring baseName = node->name;
        const size_t suffix = baseName.rfind(L" (");
        if (suffix != std::wstring::npos && baseName.back() == L')')
            baseName.resize(suffix);
        for (int n = 2; n < 100; ++n)
        {
            std::wstring candidate = baseName + L" (" + std::to_wstring(n) + L")";
            if (taken(candidate)) continue;
            node->name = std::move(candidate);
            return true;
        }
        return false;
    }

    EffectNode* GraphModel::FindNode(uint32_t nodeId)
    {
        auto it = std::ranges::find_if(m_nodes, [nodeId](const EffectNode& n) { return n.id == nodeId; });
        return it != m_nodes.end() ? &(*it) : nullptr;
    }

    const EffectNode* GraphModel::FindNode(uint32_t nodeId) const
    {
        auto it = std::ranges::find_if(m_nodes, [nodeId](const EffectNode& n) { return n.id == nodeId; });
        return it != m_nodes.end() ? &(*it) : nullptr;
    }

    // -----------------------------------------------------------------------
    // Edge management
    // -----------------------------------------------------------------------

    bool GraphModel::Connect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin)
    {
        if (srcId == dstId)
            return false;

        if (!FindNode(srcId) || !FindNode(dstId))
            return false;

        if (WouldCreateCycle(srcId, dstId))
            return false;

        // Ensure a dest input pin has at most one incoming edge.
        DisconnectInput(dstId, dstPin);

        m_edges.push_back({ srcId, srcPin, dstId, dstPin });
        FindNode(dstId)->dirty = true;
        FitVariadicPins(dstId);
        return true;
    }

    bool GraphModel::Disconnect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin)
    {
        EffectEdge target{ srcId, srcPin, dstId, dstPin };
        auto it = std::ranges::find(m_edges, target);
        if (it != m_edges.end())
        {
            m_edges.erase(it);
            if (auto* dst = FindNode(dstId))
                dst->dirty = true;
            FitVariadicPins(dstId);
            return true;
        }
        return false;
    }

    void GraphModel::DisconnectInput(uint32_t dstId, uint32_t dstPin)
    {
        const size_t removed = std::erase_if(m_edges, [dstId, dstPin](const EffectEdge& e)
        {
            return e.destNodeId == dstId && e.destPin == dstPin;
        });
        if (removed)
            if (auto* dst = FindNode(dstId)) dst->dirty = true;
        FitVariadicPins(dstId);
    }

    void GraphModel::FitVariadicPins(uint32_t nodeId)
    {
        auto* node = FindNode(nodeId);
        if (!node || !node->customEffect.has_value() || !node->customEffect->variadicInputs)
            return;
        const uint32_t maxPins = static_cast<uint32_t>(node->customEffect->inputNames.size());
        uint32_t highestPin = 0;
        bool anyConnected = false;
        for (const auto& e : m_edges)
            if (e.destNodeId == nodeId) { highestPin = (std::max)(highestPin, e.destPin); anyConnected = true; }
        const uint32_t pinCount = (std::min)(maxPins, (std::max)(2u, anyConnected ? highestPin + 2 : 2u));
        if (node->inputPins.size() == pinCount) return;
        node->inputPins.clear();
        for (uint32_t i = 0; i < pinCount; ++i)
            node->inputPins.push_back({ std::format(L"I{}", i), i });
    }

    std::vector<const EffectEdge*> GraphModel::GetInputEdges(uint32_t nodeId) const
    {
        std::vector<const EffectEdge*> result;
        for (const auto& e : m_edges)
        {
            if (e.destNodeId == nodeId)
                result.push_back(&e);
        }
        return result;
    }

    std::vector<const EffectEdge*> GraphModel::GetOutputEdges(uint32_t nodeId) const
    {
        std::vector<const EffectEdge*> result;
        for (const auto& e : m_edges)
        {
            if (e.sourceNodeId == nodeId)
                result.push_back(&e);
        }
        return result;
    }

    std::vector<uint32_t> GraphModel::GetOutputNodeIds() const
    {
        std::vector<uint32_t> ids;
        for (const auto& node : m_nodes)
        {
            if (node.type == NodeType::Output)
                ids.push_back(node.id);
        }
        return ids;
    }

    // -----------------------------------------------------------------------
    // Dependencies
    // -----------------------------------------------------------------------

    void GraphModel::ForEachDependency(DependencyKind kinds,
        const std::function<void(uint32_t sourceId, uint32_t dependentId)>& visit) const
    {
        const auto wanted = static_cast<uint8_t>(kinds);
        if (wanted & static_cast<uint8_t>(DependencyKind::Image))
        {
            for (const auto& edge : m_edges)
                visit(edge.sourceNodeId, edge.destNodeId);
        }
        if (!(wanted & static_cast<uint8_t>(DependencyKind::Binding)))
            return;

        std::unordered_set<uint32_t> ids;
        for (const auto& node : m_nodes)
            ids.insert(node.id);
        for (const auto& node : m_nodes)
        {
            std::unordered_set<uint32_t> bindingSources;
            ForEachBindingSource(node, [&](uint32_t sourceNodeId) { bindingSources.insert(sourceNodeId); });
            // A binding can name a node a hand-edited file no longer has.
            for (uint32_t srcId : bindingSources)
                if (ids.contains(srcId))
                    visit(srcId, node.id);
        }
    }

    // -----------------------------------------------------------------------
    // Topological sort (Kahn's algorithm)
    // -----------------------------------------------------------------------

    std::vector<uint32_t> GraphModel::TopologicalSort() const
    {
        std::vector<uint32_t> sorted = KahnOrder();
        if (sorted.size() != m_nodes.size())
            throw std::logic_error("GraphModel contains a cycle");
        return sorted;
    }

    std::vector<uint32_t> GraphModel::UnorderableNodes() const
    {
        const std::vector<uint32_t> sorted = KahnOrder();
        const std::unordered_set<uint32_t> ordered(sorted.begin(), sorted.end());
        std::vector<uint32_t> rest;
        for (const auto& node : m_nodes)
            if (!ordered.contains(node.id)) rest.push_back(node.id);
        return rest;
    }

    std::vector<uint32_t> GraphModel::KahnOrder() const
    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
        std::unordered_map<uint32_t, uint32_t> inDegree;

        for (const auto& node : m_nodes)
        {
            adj[node.id];            // ensure entry exists
            inDegree[node.id] = 0;
        }

        ForEachDependency(DependencyKind::Any, [&](uint32_t srcId, uint32_t dstId)
        {
            adj[srcId].push_back(dstId);
            inDegree[dstId]++;
        });

        // Seed queue with zero-in-degree nodes.
        std::queue<uint32_t> q;
        for (const auto& [id, deg] : inDegree)
        {
            if (deg == 0)
                q.push(id);
        }

        std::vector<uint32_t> sorted;
        sorted.reserve(m_nodes.size());

        while (!q.empty())
        {
            uint32_t current = q.front();
            q.pop();
            sorted.push_back(current);

            for (uint32_t neighbor : adj[current])
            {
                if (--inDegree[neighbor] == 0)
                    q.push(neighbor);
            }
        }

        return sorted;
    }

    // -----------------------------------------------------------------------
    // Cycle detection (reachability check)
    // -----------------------------------------------------------------------

    bool GraphModel::WouldCreateCycle(uint32_t srcId, uint32_t dstId) const
    {
        // If dstId already reaches srcId, adding src->dst closes a loop.
        // Bindings count: TopologicalSort orders by them, so a loop through a
        // binding leaves the graph with no evaluation order at all.
        std::unordered_map<uint32_t, std::vector<uint32_t>> downstream;
        ForEachDependency(DependencyKind::Any, [&](uint32_t from, uint32_t to)
        {
            downstream[from].push_back(to);
        });

        std::unordered_set<uint32_t> visited;
        std::vector<uint32_t> work{ dstId };
        while (!work.empty())
        {
            const uint32_t current = work.back();
            work.pop_back();
            if (current == srcId)
                return true;
            if (!visited.insert(current).second)
                continue;
            if (auto it = downstream.find(current); it != downstream.end())
                work.insert(work.end(), it->second.begin(), it->second.end());
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Property bindings
    // -----------------------------------------------------------------------

    bool GraphModel::IsBindablePropertyType(const PropertyValue& value)
    {
        return std::visit([](auto&& v) -> bool
        {
            using T = std::decay_t<decltype(v)>;
            return std::is_same_v<T, float> ||
                   std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2> ||
                   std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3> ||
                   std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4> ||
                   std::is_same_v<T, std::vector<float>>;
        }, value);
    }

    bool GraphModel::UnbindPropertySource(uint32_t nodeId, const std::wstring& propertyName,
                                           uint32_t sourceNodeId, const std::wstring& sourceFieldName)
    {
        auto* node = FindNode(nodeId);
        if (!node) return false;
        auto bindingIt = node->propertyBindings.find(propertyName);
        if (bindingIt == node->propertyBindings.end()) return false;

        auto& binding = bindingIt->second;
        bool anyLeft = false;
        if (!binding.wholeArray)
        {
            for (auto& source : binding.sources)
            {
                if (source.has_value() && source->sourceNodeId == sourceNodeId && source->sourceFieldName == sourceFieldName)
                    source.reset();
                else if (source.has_value())
                    anyLeft = true;
            }
        }
        else
        {
            anyLeft = binding.wholeArraySourceNodeId != sourceNodeId || binding.wholeArraySourceFieldName != sourceFieldName;
        }
        if (!anyLeft)
            return UnbindProperty(nodeId, propertyName);
        // Only the consumer changed; the evaluator pulls it downstream.
        node->dirty = true;
        return true;
    }

    std::wstring GraphModel::BindProperty(
        uint32_t destNodeId,
        const std::wstring& propertyName,
        uint32_t sourceNodeId,
        const std::wstring& sourceFieldName,
        uint32_t sourceComponent)
    {
        auto* destNode = FindNode(destNodeId);
        if (!destNode) return L"Destination node not found";

        auto* srcNode = FindNode(sourceNodeId);
        if (!srcNode) return L"Source node not found";

        // Verify source has typed analysis output.
        bool hasAnalysis = false;
        if (srcNode->customEffect.has_value() &&
            srcNode->customEffect->analysisOutputType == AnalysisOutputType::Typed)
            hasAnalysis = true;
        else if (srcNode->analysisOutput.type == AnalysisOutputType::Typed &&
                 !srcNode->analysisOutput.fields.empty())
            hasAnalysis = true;
        if (!hasAnalysis)
            return L"Source node has no typed analysis output";

        // Verify field exists on source and get its type.
        const AnalysisFieldDescriptor* srcField = nullptr;
        AnalysisFieldDescriptor liveField{};  // for live-data fallback
        if (srcNode->customEffect.has_value())
        {
            for (const auto& fd : srcNode->customEffect->analysisFields)
            {
                if (fd.name == sourceFieldName) { srcField = &fd; break; }
            }
        }
        // Fallback: check live analysisOutput fields (e.g., video Duration/Position).
        if (!srcField)
        {
            for (const auto& fv : srcNode->analysisOutput.fields)
            {
                if (fv.name == sourceFieldName)
                {
                    liveField.name = fv.name;
                    liveField.type = fv.type;
                    srcField = &liveField;
                    break;
                }
            }
        }
        if (!srcField) return L"Source field not found";

        // Verify destination property exists and is bindable.
        auto propIt = destNode->properties.find(propertyName);
        if (propIt == destNode->properties.end())
            return L"Property not found on destination node";
        if (!IsBindablePropertyType(propIt->second))
            return L"Property type is not bindable (must be float, float2, float3, float4, or float array)";

        // Type compatibility check.
        bool srcIsArray = AnalysisFieldIsArray(srcField->type);
        bool destIsArray = std::holds_alternative<std::vector<float>>(propIt->second);

        if (srcIsArray && !destIsArray)
            return L"Cannot bind array output to scalar property";
        if (!srcIsArray && destIsArray)
            return L"Cannot bind scalar output to array property";

        // Scalar→scalar: wider source is OK (user picks component via sourceComponent).
        // Narrower source→wider dest: replicate (float→float4 fills x,x,x,0).

        if (WouldCreateCycle(sourceNodeId, destNodeId))
            return L"Binding would create a cycle";

        // Create binding with smart defaults.
        PropertyBinding binding;

        if (destIsArray && srcIsArray)
        {
            // Whole-array passthrough.
            binding.wholeArray = true;
            binding.wholeArraySourceNodeId = sourceNodeId;
            binding.wholeArraySourceFieldName = sourceFieldName;
        }
        else
        {
            // Per-component: neither side is an array here.
            const uint32_t destCC = PropertyComponentCount(propIt->second);

            uint32_t srcCC = AnalysisFieldComponentCount(srcField->type);
            binding.sources.resize(destCC);

            if (destCC == 1)
            {
                // float dest: pick the requested component from source.
                binding.sources[0] = ComponentSource{ sourceNodeId, sourceFieldName, 0, sourceComponent };
            }
            else
            {
                // Vector dest: map 1:1 from source components.
                for (uint32_t c = 0; c < destCC; ++c)
                {
                    if (c < srcCC)
                        binding.sources[c] = ComponentSource{ sourceNodeId, sourceFieldName, 0, c };
                    // else: leave as nullopt (keeps authored default)
                }
            }
        }

        destNode->propertyBindings[propertyName] = std::move(binding);
        // The consumer is all that changed. The evaluator pulls dirtiness
        // downstream along edges and bindings, so nothing else is marked.
        destNode->dirty = true;
        return {};  // success
    }

    bool GraphModel::UnbindProperty(uint32_t nodeId, const std::wstring& propertyName)
    {
        auto* node = FindNode(nodeId);
        if (!node) return false;
        if (node->propertyBindings.erase(propertyName) == 0)
            return false;
        node->dirty = true;
        return true;
    }

    // -----------------------------------------------------------------------
    // Dirty / cache helpers
    // -----------------------------------------------------------------------

    void GraphModel::MarkAllDirty()
    {
        for (auto& node : m_nodes)
            node.dirty = true;
    }

    void GraphModel::MarkReachableDirty(std::vector<uint32_t> from)
    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> downstream;
        ForEachDependency(DependencyKind::Image, [&](uint32_t src, uint32_t dst)
        {
            downstream[src].push_back(dst);
        });

        std::unordered_set<uint32_t> visited(from.begin(), from.end());
        for (size_t i = 0; i < from.size(); ++i)
        {
            auto it = downstream.find(from[i]);
            if (it == downstream.end()) continue;
            for (uint32_t dst : it->second)
            {
                if (!visited.insert(dst).second) continue;
                if (auto* node = FindNode(dst)) node->dirty = true;
                from.push_back(dst);
            }
        }
    }

    void GraphModel::PropagateDirtyDownstream()
    {
        std::vector<uint32_t> dirty;
        for (const auto& node : m_nodes)
            if (node.dirty) dirty.push_back(node.id);
        if (!dirty.empty())
            MarkReachableDirty(std::move(dirty));
    }

    void GraphModel::MarkDownstreamDirty(uint32_t nodeId)
    {
        MarkReachableDirty({ nodeId });
    }

    void GraphModel::ComputeNeeded(std::span<const uint32_t> extraRoots)
    {
        std::vector<uint32_t> work(extraRoots.begin(), extraRoots.end());
        for (auto& node : m_nodes)
        {
            node.needed = false;
            if (node.type == NodeType::Output)
                work.push_back(node.id);
            // Data-only analysis has no consumer to pull it, so a pending
            // change demands it by itself.
            if (node.dirty && node.customEffect.has_value() &&
                node.customEffect->analysisOutputType == AnalysisOutputType::Typed)
                work.push_back(node.id);
        }

        std::unordered_map<uint32_t, std::vector<uint32_t>> upstream;
        ForEachDependency(DependencyKind::Any, [&](uint32_t src, uint32_t dst)
        {
            upstream[dst].push_back(src);
        });

        std::unordered_set<uint32_t> visited;
        while (!work.empty())
        {
            const uint32_t id = work.back();
            work.pop_back();
            if (!visited.insert(id).second) continue;
            if (auto* node = FindNode(id)) node->needed = true;
            if (auto it = upstream.find(id); it != upstream.end())
                work.insert(work.end(), it->second.begin(), it->second.end());
        }
    }

    void GraphModel::MarkAllNeeded()
    {
        for (auto& node : m_nodes)
            node.needed = true;
    }

    bool GraphModel::HasDirtyNodes() const
    {
        // Only nodes some output needs. An unneeded node keeps its pending
        // change (the evaluator does not discard it) until it becomes
        // needed again, and must not keep the host re-evaluating meanwhile.
        for (const auto& node : m_nodes)
            if (node.dirty && node.needed) return true;
        return false;
    }

    void GraphModel::ClearCachedOutputs()
    {
        for (auto& node : m_nodes)
            node.cachedOutput = nullptr;
    }

    void GraphModel::Clear()
    {
        m_nodes.clear();
        m_edges.clear();
        m_nextId = 1;
    }

    // -----------------------------------------------------------------------
    // JSON serialization helpers (anonymous namespace)
    // -----------------------------------------------------------------------
    namespace
    {
        WDJ::JsonObject PropertyValueToJson(const std::wstring& key, const PropertyValue& value)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(key));
            obj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(PropertyValueTypeTag(value)));

            std::visit([&obj](auto&& v)
            {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, float>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(v));
                }
                else if constexpr (std::is_same_v<T, int32_t>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(static_cast<double>(v)));
                }
                else if constexpr (std::is_same_v<T, uint32_t>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateNumberValue(static_cast<double>(v)));
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateBooleanValue(v));
                }
                else if constexpr (std::is_same_v<T, std::wstring>)
                {
                    obj.SetNamedValue(L"value", WDJ::JsonValue::CreateStringValue(v));
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float2>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float3>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.z));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, winrt::Windows::Foundation::Numerics::float4>)
                {
                    WDJ::JsonArray arr;
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.x));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.y));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.z));
                    arr.Append(WDJ::JsonValue::CreateNumberValue(v.w));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, D2D1_MATRIX_5X4_F>)
                {
                    // Serialize as flat 20-element array (row-major: 5 rows × 4 cols).
                    WDJ::JsonArray arr;
                    const float* p = &v._11;
                    for (int i = 0; i < 20; ++i)
                        arr.Append(WDJ::JsonValue::CreateNumberValue(p[i]));
                    obj.SetNamedValue(L"value", arr);
                }
                else if constexpr (std::is_same_v<T, std::vector<float>>)
                {
                    WDJ::JsonArray arr;
                    for (float f : v)
                        arr.Append(WDJ::JsonValue::CreateNumberValue(f));
                    obj.SetNamedValue(L"value", arr);
                }
            }, value);

            return obj;
        }

        PropertyValue PropertyValueFromJson(const WDJ::JsonObject& obj)
        {
            auto type = std::wstring(obj.GetNamedString(L"type"));

            if (type == L"float")
                return static_cast<float>(obj.GetNamedNumber(L"value"));
            if (type == L"int")
                return static_cast<int32_t>(obj.GetNamedNumber(L"value"));
            if (type == L"uint")
                return static_cast<uint32_t>(obj.GetNamedNumber(L"value"));
            if (type == L"bool")
                return obj.GetNamedBoolean(L"value");
            if (type == L"string")
                return std::wstring(obj.GetNamedString(L"value"));

            auto arr = obj.GetNamedArray(L"value");
            if (type == L"float2")
            {
                return winrt::Windows::Foundation::Numerics::float2{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1))
                };
            }
            if (type == L"float3")
            {
                return winrt::Windows::Foundation::Numerics::float3{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1)),
                    static_cast<float>(arr.GetNumberAt(2))
                };
            }
            if (type == L"float4")
            {
                return winrt::Windows::Foundation::Numerics::float4{
                    static_cast<float>(arr.GetNumberAt(0)),
                    static_cast<float>(arr.GetNumberAt(1)),
                    static_cast<float>(arr.GetNumberAt(2)),
                    static_cast<float>(arr.GetNumberAt(3))
                };
            }
            if (type == L"matrix5x4")
            {
                D2D1_MATRIX_5X4_F m{};
                float* p = &m._11;
                uint32_t count = (std::min)(arr.Size(), 20u);
                for (uint32_t i = 0; i < count; ++i)
                    p[i] = static_cast<float>(arr.GetNumberAt(i));
                return m;
            }
            if (type == L"floatarray")
            {
                std::vector<float> v;
                v.reserve(arr.Size());
                for (uint32_t i = 0; i < arr.Size(); ++i)
                    v.push_back(static_cast<float>(arr.GetNumberAt(i)));
                return v;
            }

            throw std::invalid_argument("Unknown property type in JSON");
        }

        // The JSON key a Source node's media path is saved under, and the
        // property older graphs mirrored it into.
        constexpr const wchar_t* cMediaPathKey = L"shaderPath";

        void ContractsToJson(const CustomEffectDefinition& def, WDJ::JsonObject& ced)
        {
            ced.SetNamedValue(L"nodeKind", WDJ::JsonValue::CreateStringValue(NodeKindTag(def.nodeKind)));
            ced.SetNamedValue(L"dispatchShape", WDJ::JsonValue::CreateStringValue(DispatchShapeTag(def.dispatchShape)));
            WDJ::JsonObject extent;
            extent.SetNamedValue(L"sizeParam", WDJ::JsonValue::CreateStringValue(def.outputExtent.sizeParam));
            extent.SetNamedValue(L"columns", WDJ::JsonValue::CreateNumberValue(def.outputExtent.columns));
            extent.SetNamedValue(L"rows", WDJ::JsonValue::CreateNumberValue(def.outputExtent.rows));
            ced.SetNamedValue(L"outputExtent", extent);
            WDJ::JsonArray injected;
            for (const auto& tag : InjectedConstantTags(def.injectedConstants))
                injected.Append(WDJ::JsonValue::CreateStringValue(tag));
            ced.SetNamedValue(L"injectedConstants", injected);
        }

        // Reads the contracts, deriving any that a graph saved before them
        // lacks. Runs after the rest of the definition: derivation reads it.
        void ContractsFromJson(const WDJ::JsonObject& ced, const std::map<std::wstring, PropertyValue>& properties,
                               CustomEffectDefinition& def, std::vector<std::wstring>& errors)
        {
            def.nodeKind = DeriveNodeKind(def);
            if (ced.HasKey(L"nodeKind"))
            {
                const std::wstring tag(ced.GetNamedString(L"nodeKind"));
                if (auto kind = NodeKindFromTag(tag)) def.nodeKind = *kind;
                else errors.push_back(L"unknown nodeKind '" + tag + L"'");
            }

            def.dispatchShape = DeriveDispatchShape(def);
            if (ced.HasKey(L"dispatchShape"))
            {
                const std::wstring tag(ced.GetNamedString(L"dispatchShape"));
                if (auto shape = DispatchShapeFromTag(tag)) def.dispatchShape = *shape;
                else errors.push_back(L"unknown dispatchShape '" + tag + L"'");
            }

            if (ced.HasKey(L"outputExtent"))
            {
                const auto extent = ced.GetNamedObject(L"outputExtent");
                def.outputExtent.sizeParam = std::wstring(extent.GetNamedString(L"sizeParam", L""));
                // A multiplier outside 1..16384 would size the output 0 or wrap.
                auto readMultiplier = [&](const wchar_t* key) -> uint32_t
                {
                    const double value = extent.GetNamedNumber(key, 1.0);
                    if (value >= 1.0 && value <= 16384.0) return static_cast<uint32_t>(value);
                    errors.push_back(std::format(L"outputExtent {} {} out of range, read as 1", key, value));
                    return 1;
                };
                def.outputExtent.columns = readMultiplier(L"columns");
                def.outputExtent.rows = readMultiplier(L"rows");
            }
            else
            {
                def.outputExtent = DeriveOutputExtent(def, properties);
            }

            if (ced.HasKey(L"injectedConstants"))
            {
                const auto tags = ced.GetNamedArray(L"injectedConstants");
                for (uint32_t i = 0; i < tags.Size(); ++i)
                {
                    const std::wstring tag(tags.GetStringAt(i));
                    if (auto flag = InjectedConstantFromTag(tag)) def.injectedConstants = def.injectedConstants | *flag;
                    else errors.push_back(L"unknown injected constant '" + tag + L"'");
                }
            }
            else
            {
                def.injectedConstants = DeriveInjectedConstants(properties);
            }
        }

        WDJ::JsonObject NodeToJson(const EffectNode& node)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"id", WDJ::JsonValue::CreateNumberValue(node.id));
            obj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(node.name));
            obj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(NodeTypeToString(node.type)));

            // Position
            WDJ::JsonArray pos;
            pos.Append(WDJ::JsonValue::CreateNumberValue(node.position.x));
            pos.Append(WDJ::JsonValue::CreateNumberValue(node.position.y));
            obj.SetNamedValue(L"position", pos);

            // Properties
            WDJ::JsonArray props;
            for (const auto& [key, value] : node.properties)
            {
                props.Append(PropertyValueToJson(key, value));
            }
            obj.SetNamedValue(L"properties", props);

            // Property bindings.
            if (!node.propertyBindings.empty())
            {
                WDJ::JsonObject bindingsObj;
                for (const auto& [propName, binding] : node.propertyBindings)
                {
                    WDJ::JsonObject bobj;
                    if (binding.wholeArray)
                    {
                        bobj.SetNamedValue(L"wholeArray", WDJ::JsonValue::CreateBooleanValue(true));
                        bobj.SetNamedValue(L"sourceNodeId", WDJ::JsonValue::CreateNumberValue(binding.wholeArraySourceNodeId));
                        bobj.SetNamedValue(L"sourceFieldName", WDJ::JsonValue::CreateStringValue(binding.wholeArraySourceFieldName));
                    }
                    else
                    {
                        WDJ::JsonArray srcs;
                        for (const auto& src : binding.sources)
                        {
                            if (src.has_value())
                            {
                                WDJ::JsonObject sobj;
                                sobj.SetNamedValue(L"nodeId", WDJ::JsonValue::CreateNumberValue(src->sourceNodeId));
                                sobj.SetNamedValue(L"field", WDJ::JsonValue::CreateStringValue(src->sourceFieldName));
                                sobj.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(src->sourceIndex));
                                sobj.SetNamedValue(L"comp", WDJ::JsonValue::CreateNumberValue(src->sourceComponent));
                                srcs.Append(sobj);
                            }
                            else
                            {
                                srcs.Append(WDJ::JsonValue::CreateNullValue());
                            }
                        }
                        bobj.SetNamedValue(L"sources", srcs);
                    }
                    bindingsObj.SetNamedValue(propName, bobj);
                }
                obj.SetNamedValue(L"propertyBindings", bindingsObj);
            }

            // Effect CLSID (stored as string for portability)
            if (node.effectClsid.has_value())
            {
                wchar_t guidStr[64]{};
                StringFromGUID2(node.effectClsid.value(), guidStr, 64);
                obj.SetNamedValue(L"effectClsid", WDJ::JsonValue::CreateStringValue(guidStr));
            }

            // Media path, under its original key.
            if (node.mediaPath.has_value())
                obj.SetNamedValue(cMediaPathKey, WDJ::JsonValue::CreateStringValue(node.mediaPath.value()));

            // Pins
            WDJ::JsonArray inPins;
            for (const auto& pin : node.inputPins)
            {
                WDJ::JsonObject p;
                p.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(pin.name));
                p.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(pin.index));
                inPins.Append(p);
            }
            obj.SetNamedValue(L"inputPins", inPins);

            WDJ::JsonArray outPins;
            for (const auto& pin : node.outputPins)
            {
                WDJ::JsonObject p;
                p.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(pin.name));
                p.SetNamedValue(L"index", WDJ::JsonValue::CreateNumberValue(pin.index));
                outPins.Append(p);
            }
            obj.SetNamedValue(L"outputPins", outPins);

            // Custom effect definition.
            if (node.customEffect.has_value())
            {
                WDJ::JsonObject ced;
                auto& def = node.customEffect.value();
                ced.SetNamedValue(L"shaderType", WDJ::JsonValue::CreateNumberValue(
                    static_cast<double>(def.shaderType)));
                ced.SetNamedValue(L"hlslSource", WDJ::JsonValue::CreateStringValue(def.hlslSource));

                WDJ::JsonArray inputs;
                for (const auto& name : def.inputNames)
                    inputs.Append(WDJ::JsonValue::CreateStringValue(name));
                ced.SetNamedValue(L"inputNames", inputs);

                WDJ::JsonArray params;
                for (const auto& p : def.parameters)
                {
                    WDJ::JsonObject po;
                    po.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(p.name));
                    po.SetNamedValue(L"typeName", WDJ::JsonValue::CreateStringValue(p.typeName));
                    po.SetNamedValue(L"minValue", WDJ::JsonValue::CreateNumberValue(p.minValue));
                    po.SetNamedValue(L"maxValue", WDJ::JsonValue::CreateNumberValue(p.maxValue));
                    po.SetNamedValue(L"step", WDJ::JsonValue::CreateNumberValue(p.step));
                    po.SetNamedValue(L"default", PropertyValueToJson(p.name, p.defaultValue));
                    if (!p.enumLabels.empty())
                    {
                        WDJ::JsonArray labels;
                        for (const auto& label : p.enumLabels)
                            labels.Append(WDJ::JsonValue::CreateStringValue(label));
                        po.SetNamedValue(L"enumLabels", labels);
                    }
                    if (!p.visibleWhen.empty())
                        po.SetNamedValue(L"visibleWhen", WDJ::JsonValue::CreateStringValue(p.visibleWhen));
                    if (p.gpuBindable)
                        po.SetNamedValue(L"gpuBindable", WDJ::JsonValue::CreateBooleanValue(true));
                    if (p.specialize)
                        po.SetNamedValue(L"specialize", WDJ::JsonValue::CreateBooleanValue(true));
                    params.Append(po);
                }
                ced.SetNamedValue(L"parameters", params);

                if (def.lookupInputCount > 0)
                    ced.SetNamedValue(L"lookupInputCount",
                        WDJ::JsonValue::CreateNumberValue(def.lookupInputCount));
                if (def.variadicInputs)
                    ced.SetNamedValue(L"variadicInputs", WDJ::JsonValue::CreateBooleanValue(true));
                ced.SetNamedValue(L"threadGroupX", WDJ::JsonValue::CreateNumberValue(def.threadGroupX));
                ced.SetNamedValue(L"threadGroupY", WDJ::JsonValue::CreateNumberValue(def.threadGroupY));
                ced.SetNamedValue(L"threadGroupZ", WDJ::JsonValue::CreateNumberValue(def.threadGroupZ));
                ced.SetNamedValue(L"analysisOutputType", WDJ::JsonValue::CreateNumberValue(
                    static_cast<double>(def.analysisOutputType)));
                // Unused; written because earlier builds require the key to load.
                ced.SetNamedValue(L"analysisOutputSize", WDJ::JsonValue::CreateNumberValue(256));

                // Serialize typed analysis fields.
                if (!def.analysisFields.empty())
                {
                    WDJ::JsonArray fields;
                    for (const auto& fd : def.analysisFields)
                    {
                        WDJ::JsonObject fobj;
                        fobj.SetNamedValue(L"name", WDJ::JsonValue::CreateStringValue(fd.name));
                        // Serialize type as string tag.
                        fobj.SetNamedValue(L"type", WDJ::JsonValue::CreateStringValue(AnalysisFieldTypeTag(fd.type)));
                        if (AnalysisFieldIsArray(fd.type))
                            fobj.SetNamedValue(L"length", WDJ::JsonValue::CreateNumberValue(fd.arrayLength));
                        if (fd.gpuPublish)
                            fobj.SetNamedValue(L"gpuPublish", WDJ::JsonValue::CreateBooleanValue(true));
                        fields.Append(fobj);
                    }
                    ced.SetNamedValue(L"analysisFields", fields);
                }

                // ShaderLab effect identity (for version upgrade detection).
                if (!def.shaderLabEffectId.empty())
                {
                    ced.SetNamedValue(L"shaderLabEffectId", WDJ::JsonValue::CreateStringValue(def.shaderLabEffectId));
                    ced.SetNamedValue(L"shaderLabEffectVersion", WDJ::JsonValue::CreateNumberValue(def.shaderLabEffectVersion));
                }

                ContractsToJson(def, ced);

                obj.SetNamedValue(L"customEffect", ced);
            }

            return obj;
        }

        EffectNode NodeFromJson(const WDJ::JsonObject& obj)
        {
            EffectNode node;
            node.id = static_cast<uint32_t>(obj.GetNamedNumber(L"id"));
            node.name = std::wstring(obj.GetNamedString(L"name"));
            node.type = NodeTypeFromString(std::wstring(obj.GetNamedString(L"type")));

            auto pos = obj.GetNamedArray(L"position");
            node.position = {
                static_cast<float>(pos.GetNumberAt(0)),
                static_cast<float>(pos.GetNumberAt(1))
            };

            auto props = obj.GetNamedArray(L"properties");
            std::wstring legacyAnalysisFieldsJson;
            std::wstring legacyPropertyBindingsJson;
            std::vector<std::wstring> unknownFieldTypes;
            std::vector<std::wstring> loadErrors;
            for (uint32_t i = 0; i < props.Size(); ++i)
            {
                auto propObj = props.GetObjectAt(i);
                auto key = std::wstring(propObj.GetNamedString(L"name"));
                // Extract legacy internal metadata stored as string properties.
                if (key == L"analysisFields")
                {
                    if (propObj.HasKey(L"value"))
                        legacyAnalysisFieldsJson = std::wstring(propObj.GetNamedString(L"value"));
                    continue;
                }
                if (key == L"propertyBindings")
                {
                    if (propObj.HasKey(L"value"))
                        legacyPropertyBindingsJson = std::wstring(propObj.GetNamedString(L"value"));
                    continue;
                }
                node.properties[key] = PropertyValueFromJson(propObj);
            }

            // Property bindings.
            if (obj.HasKey(L"propertyBindings"))
            {
                auto bindingsObj = obj.GetNamedObject(L"propertyBindings");
                for (const auto& pair : bindingsObj)
                {
                    auto propName = std::wstring(pair.Key());
                    auto bobj = pair.Value().GetObject();
                    PropertyBinding binding;

                    if (bobj.HasKey(L"wholeArray") && bobj.GetNamedBoolean(L"wholeArray"))
                    {
                        // New whole-array format.
                        binding.wholeArray = true;
                        binding.wholeArraySourceNodeId = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceNodeId"));
                        binding.wholeArraySourceFieldName = std::wstring(bobj.GetNamedString(L"sourceFieldName"));
                    }
                    else if (bobj.HasKey(L"sources"))
                    {
                        // New per-component format.
                        auto srcs = bobj.GetNamedArray(L"sources");
                        for (uint32_t si = 0; si < srcs.Size(); ++si)
                        {
                            if (srcs.GetAt(si).ValueType() == WDJ::JsonValueType::Null)
                            {
                                binding.sources.push_back(std::nullopt);
                            }
                            else
                            {
                                auto sobj = srcs.GetObjectAt(si);
                                ComponentSource cs;
                                cs.sourceNodeId = static_cast<uint32_t>(sobj.GetNamedNumber(L"nodeId"));
                                cs.sourceFieldName = std::wstring(sobj.GetNamedString(L"field"));
                                cs.sourceIndex = static_cast<uint32_t>(sobj.GetNamedNumber(L"index"));
                                cs.sourceComponent = static_cast<uint32_t>(sobj.GetNamedNumber(L"comp"));
                                binding.sources.push_back(cs);
                            }
                        }
                    }
                    else if (bobj.HasKey(L"sourceNodeId"))
                    {
                        // Legacy single-source format → migrate.
                        uint32_t srcNodeId = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceNodeId"));
                        auto srcField = std::wstring(bobj.GetNamedString(L"sourceFieldName"));
                        uint32_t srcComp = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceComponent"));

                        // Determine dest component count from property.
                        auto propIt = node.properties.find(propName);
                        uint32_t destCC = 1;
                        if (propIt != node.properties.end())
                        {
                            destCC = PropertyComponentCount(propIt->second);
                        }

                        if (destCC == 0)
                        {
                            binding.wholeArray = true;
                            binding.wholeArraySourceNodeId = srcNodeId;
                            binding.wholeArraySourceFieldName = srcField;
                        }
                        else if (destCC == 1)
                        {
                            binding.sources.push_back(ComponentSource{ srcNodeId, srcField, 0, srcComp });
                        }
                        else
                        {
                            binding.sources.resize(destCC);
                            for (uint32_t c = 0; c < destCC; ++c)
                                binding.sources[c] = ComponentSource{ srcNodeId, srcField, 0, c };
                        }
                    }

                    node.propertyBindings[propName] = std::move(binding);
                }
            }

            if (obj.HasKey(L"effectClsid"))
            {
                const std::wstring clsidText(obj.GetNamedString(L"effectClsid"));
                GUID guid{};
                if (SUCCEEDED(CLSIDFromString(clsidText.c_str(), &guid)))
                    node.effectClsid = guid;
                else
                    node.runtimeError = L"effectClsid '" + clsidText + L"' is not a valid CLSID.";
            }

            if (obj.HasKey(cMediaPathKey))
                node.mediaPath = std::wstring(obj.GetNamedString(cMediaPathKey));
            // Older graphs also kept the path as a "shaderPath" property, and
            // a video read that copy first.
            if (auto it = node.properties.find(cMediaPathKey); it != node.properties.end())
            {
                if (const auto* path = std::get_if<std::wstring>(&it->second); path && !path->empty())
                    node.mediaPath = *path;
                node.properties.erase(it);
            }

            // Pins
            {
                auto arr = obj.GetNamedArray(L"inputPins");
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    auto p = arr.GetObjectAt(i);
                    node.inputPins.push_back({
                        std::wstring(p.GetNamedString(L"name")),
                        static_cast<uint32_t>(p.GetNamedNumber(L"index"))
                    });
                }
            }
            {
                auto arr = obj.GetNamedArray(L"outputPins");
                for (uint32_t i = 0; i < arr.Size(); ++i)
                {
                    auto p = arr.GetObjectAt(i);
                    node.outputPins.push_back({
                        std::wstring(p.GetNamedString(L"name")),
                        static_cast<uint32_t>(p.GetNamedNumber(L"index"))
                    });
                }
            }

            // Custom effect definition.
            if (obj.HasKey(L"customEffect"))
            {
                auto ced = obj.GetNamedObject(L"customEffect");
                CustomEffectDefinition def;
                const double shaderType = ced.GetNamedNumber(L"shaderType");
                if (shaderType == 0.0 || shaderType == 1.0 || shaderType == 2.0)
                    def.shaderType = static_cast<CustomShaderType>(static_cast<int>(shaderType));
                else
                    loadErrors.push_back(std::format(L"unknown shaderType {}, read as a pixel shader", shaderType));
                def.hlslSource = std::wstring(ced.GetNamedString(L"hlslSource"));

                auto inputs = ced.GetNamedArray(L"inputNames");
                for (uint32_t i = 0; i < inputs.Size(); ++i)
                    def.inputNames.push_back(std::wstring(inputs.GetStringAt(i)));

                auto params = ced.GetNamedArray(L"parameters");
                for (uint32_t i = 0; i < params.Size(); ++i)
                {
                    auto po = params.GetObjectAt(i);
                    ParameterDefinition pd;
                    pd.name = std::wstring(po.GetNamedString(L"name"));
                    pd.typeName = std::wstring(po.GetNamedString(L"typeName"));
                    pd.minValue = static_cast<float>(po.GetNamedNumber(L"minValue"));
                    pd.maxValue = static_cast<float>(po.GetNamedNumber(L"maxValue"));
                    pd.step = static_cast<float>(po.GetNamedNumber(L"step"));
                    if (po.HasKey(L"default"))
                        pd.defaultValue = PropertyValueFromJson(po.GetNamedObject(L"default"));
                    if (po.HasKey(L"enumLabels"))
                    {
                        auto labels = po.GetNamedArray(L"enumLabels");
                        for (uint32_t j = 0; j < labels.Size(); ++j)
                            pd.enumLabels.push_back(std::wstring(labels.GetStringAt(j)));
                    }
                    if (po.HasKey(L"visibleWhen"))
                        pd.visibleWhen = std::wstring(po.GetNamedString(L"visibleWhen"));
                    if (po.HasKey(L"gpuBindable"))
                        pd.gpuBindable = po.GetNamedBoolean(L"gpuBindable");
                    if (po.HasKey(L"specialize"))
                        pd.specialize = po.GetNamedBoolean(L"specialize");
                    def.parameters.push_back(std::move(pd));
                }

                if (ced.HasKey(L"lookupInputCount"))
                    def.lookupInputCount = static_cast<uint32_t>(ced.GetNamedNumber(L"lookupInputCount"));
                if (ced.HasKey(L"variadicInputs"))
                    def.variadicInputs = ced.GetNamedBoolean(L"variadicInputs");
                def.threadGroupX = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupX"));
                def.threadGroupY = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupY"));
                def.threadGroupZ = static_cast<uint32_t>(ced.GetNamedNumber(L"threadGroupZ"));
                const double outputType = ced.GetNamedNumber(L"analysisOutputType");
                if (outputType == 0.0 || outputType == 1.0 || outputType == 3.0)
                    def.analysisOutputType = static_cast<AnalysisOutputType>(static_cast<int>(outputType));
                else if (outputType != 2.0)
                    loadErrors.push_back(std::format(L"unknown analysisOutputType {}, read as none", outputType));

                // Deserialize typed analysis fields (new format).
                if (ced.HasKey(L"analysisFields"))
                {
                    auto fields = ced.GetNamedArray(L"analysisFields");
                    for (uint32_t fi = 0; fi < fields.Size(); ++fi)
                    {
                        auto fobj = fields.GetObjectAt(fi);
                        AnalysisFieldDescriptor fd;
                        fd.name = std::wstring(fobj.GetNamedString(L"name"));
                        auto typeTag = std::wstring(fobj.GetNamedString(L"type"));
                        if (auto type = AnalysisFieldTypeFromTag(typeTag))
                            fd.type = *type;
                        else
                            unknownFieldTypes.push_back(fd.name + L" (" + typeTag + L")");
                        if (fobj.HasKey(L"length"))
                            fd.arrayLength = static_cast<uint32_t>(fobj.GetNamedNumber(L"length"));
                        if (fobj.HasKey(L"gpuPublish"))
                            fd.gpuPublish = fobj.GetNamedBoolean(L"gpuPublish");
                        def.analysisFields.push_back(std::move(fd));
                    }
                }
                // Legacy: convert old analysisFieldNames (all float4) to typed fields.
                else if (ced.HasKey(L"analysisFieldNames"))
                {
                    auto fields = ced.GetNamedArray(L"analysisFieldNames");
                    for (uint32_t fi = 0; fi < fields.Size(); ++fi)
                    {
                        AnalysisFieldDescriptor fd;
                        fd.name = std::wstring(fields.GetStringAt(fi));
                        fd.type = AnalysisFieldType::Float4;
                        def.analysisFields.push_back(std::move(fd));
                    }
                }

                // Infer Typed output type when fields exist but type wasn't set.
                if (!def.analysisFields.empty() && def.analysisOutputType == AnalysisOutputType::None)
                    def.analysisOutputType = AnalysisOutputType::Typed;

                // ShaderLab effect identity (for version upgrade detection).
                if (ced.HasKey(L"shaderLabEffectId"))
                    def.shaderLabEffectId = std::wstring(ced.GetNamedString(L"shaderLabEffectId"));
                if (ced.HasKey(L"shaderLabEffectVersion"))
                    def.shaderLabEffectVersion = static_cast<uint32_t>(ced.GetNamedNumber(L"shaderLabEffectVersion"));

                ContractsFromJson(ced, node.properties, def, loadErrors);

                // The evaluator compiles hlslSource when it first needs the node.
                CoCreateGuid(&def.shaderGuid);
                node.customEffect = std::move(def);
            }

            // Migrate legacy analysisFields from string property to customEffect.
            if (!legacyAnalysisFieldsJson.empty() && node.customEffect.has_value() &&
                node.customEffect->analysisFields.empty())
            {
                try
                {
                    auto arr = WDJ::JsonArray::Parse(legacyAnalysisFieldsJson);
                    for (uint32_t fi = 0; fi < arr.Size(); ++fi)
                    {
                        auto fobj = arr.GetObjectAt(fi);
                        AnalysisFieldDescriptor fd;
                        fd.name = std::wstring(fobj.GetNamedString(L"name"));
                        auto typeTag = std::wstring(fobj.GetNamedString(L"type"));
                        if (auto type = AnalysisFieldTypeFromTag(typeTag))
                            fd.type = *type;
                        else
                            unknownFieldTypes.push_back(fd.name + L" (" + typeTag + L")");
                        if (fobj.HasKey(L"length"))
                            fd.arrayLength = static_cast<uint32_t>(fobj.GetNamedNumber(L"length"));
                        node.customEffect->analysisFields.push_back(std::move(fd));
                    }
                    if (!node.customEffect->analysisFields.empty())
                        node.customEffect->analysisOutputType = AnalysisOutputType::Typed;
                }
                catch (...)
                {
                    // Malformed legacy data. Keep loading -- the rest of the
                    // node is valid -- but say so: a silently dropped analysis
                    // field otherwise shows up later as a binding that cannot
                    // resolve, with nothing pointing back at the load.
                    node.runtimeError =
                        L"Legacy analysis-field metadata was malformed and "
                        L"could not be migrated; re-save this graph.";
                }
            }

            // Migrate legacy propertyBindings from string property.
            if (!legacyPropertyBindingsJson.empty() && node.propertyBindings.empty())
            {
                try
                {
                    auto arr = WDJ::JsonArray::Parse(legacyPropertyBindingsJson);
                    for (uint32_t bi = 0; bi < arr.Size(); ++bi)
                    {
                        auto bobj = arr.GetObjectAt(bi);
                        PropertyBinding binding;
                        uint32_t srcNodeId = static_cast<uint32_t>(bobj.GetNamedNumber(L"sourceNodeId"));
                        auto srcField = std::wstring(bobj.GetNamedString(L"sourceField"));
                        uint32_t srcComp = static_cast<uint32_t>(bobj.GetNamedNumber(L"component"));
                        binding.sources.push_back(ComponentSource{ srcNodeId, srcField, 0, srcComp });
                        auto targetProp = std::wstring(bobj.GetNamedString(L"targetProperty"));
                        node.propertyBindings[targetProp] = std::move(binding);
                    }
                }
                catch (...)
                {
                    // Partial migration: bindings parsed before the throw are
                    // kept, the rest are lost. Record it -- dropping a user's
                    // bindings silently on load is indistinguishable from
                    // never having authored them.
                    node.runtimeError =
                        L"Legacy property bindings were malformed; some "
                        L"bindings could not be migrated and must be re-made.";
                }
            }

            // An unknown field type loads as float4, which can misread every
            // field after it; say which ones.
            if (!unknownFieldTypes.empty() && node.runtimeError.empty())
            {
                node.runtimeError = L"Unknown analysis field type, read as float4:";
                for (const auto& field : unknownFieldTypes)
                    node.runtimeError += L" " + field;
            }
            if (!loadErrors.empty() && node.runtimeError.empty())
            {
                node.runtimeError = L"Load:";
                for (const auto& error : loadErrors)
                    node.runtimeError += L" " + error + L";";
            }

            node.dirty = true;
            return node;
        }

        WDJ::JsonObject EdgeToJson(const EffectEdge& edge)
        {
            WDJ::JsonObject obj;
            obj.SetNamedValue(L"sourceNodeId", WDJ::JsonValue::CreateNumberValue(edge.sourceNodeId));
            obj.SetNamedValue(L"sourcePin", WDJ::JsonValue::CreateNumberValue(edge.sourcePin));
            obj.SetNamedValue(L"destNodeId", WDJ::JsonValue::CreateNumberValue(edge.destNodeId));
            obj.SetNamedValue(L"destPin", WDJ::JsonValue::CreateNumberValue(edge.destPin));
            return obj;
        }

        EffectEdge EdgeFromJson(const WDJ::JsonObject& obj)
        {
            return EffectEdge{
                .sourceNodeId = static_cast<uint32_t>(obj.GetNamedNumber(L"sourceNodeId")),
                .sourcePin    = static_cast<uint32_t>(obj.GetNamedNumber(L"sourcePin")),
                .destNodeId   = static_cast<uint32_t>(obj.GetNamedNumber(L"destNodeId")),
                .destPin      = static_cast<uint32_t>(obj.GetNamedNumber(L"destPin")),
            };
        }
    }

    // -----------------------------------------------------------------------
    // JSON round-trip
    // -----------------------------------------------------------------------

    winrt::hstring GraphModel::ToJson() const
    {
        WDJ::JsonObject root;

        // Version metadata
        root.SetNamedValue(L"formatVersion", WDJ::JsonValue::CreateNumberValue(cGraphFormatVersion));
        root.SetNamedValue(L"effectGraphVersion",
            WDJ::JsonValue::CreateStringValue(winrt::to_hstring(EFFECTGRAPH_VERSION_STRING)));

        // Nodes
        WDJ::JsonArray nodesArr;
        for (const auto& node : m_nodes)
            nodesArr.Append(NodeToJson(node));
        root.SetNamedValue(L"nodes", nodesArr);

        // Edges
        WDJ::JsonArray edgesArr;
        for (const auto& edge : m_edges)
            edgesArr.Append(EdgeToJson(edge));
        root.SetNamedValue(L"edges", edgesArr);

        // Next ID (so deserialized graphs can continue adding nodes)
        root.SetNamedValue(L"nextId", WDJ::JsonValue::CreateNumberValue(m_nextId));

        return root.Stringify();
    }

    GraphModel GraphModel::FromJson(winrt::hstring const& json)
    {
        auto root = WDJ::JsonObject::Parse(json);

        // Check graph format version compatibility.
        if (root.HasKey(L"formatVersion"))
        {
            uint32_t fileVersion = static_cast<uint32_t>(root.GetNamedNumber(L"formatVersion"));
            if (fileVersion > cGraphFormatVersion)
            {
                // Graphs saved before EffectGraph had its own version carry ShaderLab's.
                std::wstring savedBy;
                if (root.HasKey(L"effectGraphVersion"))
                    savedBy = L"EffectGraph " + std::wstring(root.GetNamedString(L"effectGraphVersion"));
                else if (root.HasKey(L"appVersion"))
                    savedBy = L"ShaderLab " + std::wstring(root.GetNamedString(L"appVersion"));
                throw std::runtime_error(
                    "This graph uses format " + std::to_string(fileVersion) + ", newer than this EffectGraph "
                    EFFECTGRAPH_VERSION_STRING " reads (" + std::to_string(cGraphFormatVersion) + ")"
                    + (savedBy.empty() ? std::string{} : "; it was saved by " + winrt::to_string(savedBy))
                    + ". Update to open it.");
            }
        }

        GraphModel graph;

        std::unordered_set<uint32_t> ids;
        uint32_t maxId = 0;
        auto nodesArr = root.GetNamedArray(L"nodes");
        for (uint32_t i = 0; i < nodesArr.Size(); ++i)
        {
            EffectNode node = NodeFromJson(nodesArr.GetObjectAt(i));
            // Edges and bindings name nodes by id, so two nodes sharing
            // one leave the graph ambiguous.
            if (!ids.insert(node.id).second)
                throw std::runtime_error(std::format("Graph has two nodes with id {}.", node.id));
            maxId = (std::max)(maxId, node.id);
            graph.m_nodes.push_back(std::move(node));
        }

        auto appendError = [&graph](uint32_t nodeId, const std::wstring& error)
        {
            auto* node = graph.FindNode(nodeId);
            if (!node) return;
            if (!node->runtimeError.empty()) node->runtimeError += L" ";
            node->runtimeError += error;
        };

        // An edge to or from a node the file lacks is dropped; the node at
        // the other end says so.
        auto edgesArr = root.GetNamedArray(L"edges");
        for (uint32_t i = 0; i < edgesArr.Size(); ++i)
        {
            const EffectEdge edge = EdgeFromJson(edgesArr.GetObjectAt(i));
            const bool hasSource = ids.contains(edge.sourceNodeId);
            const bool hasDest = ids.contains(edge.destNodeId);
            if (hasSource && hasDest)
                graph.m_edges.push_back(edge);
            else if (hasDest)
                appendError(edge.destNodeId, std::format(L"Load: input {} named missing node {}; dropped.",
                    edge.destPin, edge.sourceNodeId));
            else if (hasSource)
                appendError(edge.sourceNodeId, std::format(L"Load: output to missing node {} dropped.",
                    edge.destNodeId));
        }

        // So is a binding that reads a missing node.
        for (auto& node : graph.m_nodes)
        {
            std::vector<std::wstring> dropped;
            std::erase_if(node.propertyBindings, [&](const auto& entry)
            {
                if (!entry.second.AnySourceNode([&](uint32_t sourceId) { return !ids.contains(sourceId); }))
                    return false;
                dropped.push_back(entry.first);
                return true;
            });
            for (const auto& property : dropped)
                appendError(node.id, L"Load: binding of '" + property + L"' named a missing node; dropped.");
        }

        // AddNode must not hand out an id the file already uses.
        const uint32_t savedNextId = static_cast<uint32_t>(root.GetNamedNumber(L"nextId", 1.0));
        graph.m_nextId = (std::max)(savedNextId, maxId + 1);

        // A variadic node's pins derive from its edges.
        for (const auto& n : graph.m_nodes)
            graph.FitVariadicPins(n.id);

        // The evaluator replaces runtimeError on the node's first successful
        // evaluate, so load diagnostics also go to the logs.
        for (const auto& n : graph.m_nodes)
        {
            if (n.runtimeError.empty()) continue;
            const std::wstring line = std::format(L"[ShaderLab] load: node {} ({}): {}\n", n.id, n.name, n.runtimeError);
            std::fputws(line.c_str(), stderr);
            ::OutputDebugStringW(line.c_str());
        }

        return graph;
    }
}
