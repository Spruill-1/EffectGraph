#pragma once

#include "pch_engine.h"
#include "EffectNode.h"
#include "EffectEdge.h"

#include <span>

namespace ShaderLab::Graph
{
    // Saved graphs' format. Increment when the JSON changes in a way an older
    // engine cannot safely load; a newer format is refused on load.
    constexpr uint32_t cGraphFormatVersion = 2;

    // The two ways one node depends on another: an image edge into one of its
    // input pins, or a property binding that reads one of its analysis fields.
    enum class DependencyKind : uint8_t
    {
        Image   = 1,
        Binding = 2,
        Any     = Image | Binding,
    };

    // DAG-based effect graph.
    // Owns nodes and edges; provides topological ordering and JSON round-trip.
    class GraphModel
    {
    public:
        GraphModel() = default;

        // --- Node management ---

        // Adds a node and returns its assigned ID.
        uint32_t AddNode(EffectNode node);

        // Removes a node and all edges connected to it.
        void RemoveNode(uint32_t nodeId);

        // Returns a pointer to the node, or nullptr if not found. Nodes live
        // in a vector: AddNode, RemoveNode, Clear and assignment invalidate
        // the pointer, so hold an id across them, not a pointer.
        EffectNode* FindNode(uint32_t nodeId);
        const EffectNode* FindNode(uint32_t nodeId) const;

        // A custom effect's name stands for its shader. When another custom
        // effect node has this node's name but different HLSL, renames this
        // node "Name (N)" with the lowest N from 2 to 99 that no node with
        // different HLSL holds (a trailing " (N)" is replaced). Returns true
        // when it renamed.
        bool EnsureUniqueCustomEffectName(uint32_t nodeId);

        // --- Edge management ---

        // Connects an output pin of the source node to an input pin of the dest node.
        // Returns false if the connection would create a cycle.
        bool Connect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin);

        // Removes a specific edge. Returns true if found and removed.
        bool Disconnect(uint32_t srcId, uint32_t srcPin, uint32_t dstId, uint32_t dstPin);

        // Removes all edges connected to a given input pin (ensures single input).
        void DisconnectInput(uint32_t dstId, uint32_t dstPin);

        // Size a variadic node's pins to the highest connected pin plus one
        // spare, between 2 and the declared maximum. Call it after replacing
        // a node's pins (e.g. an effect upgrade); edge edits call it already.
        void FitVariadicPins(uint32_t nodeId);

        // --- Queries ---

        // Returns edges that feed into a given node.
        std::vector<const EffectEdge*> GetInputEdges(uint32_t nodeId) const;

        // Returns edges that originate from a given node.
        std::vector<const EffectEdge*> GetOutputEdges(uint32_t nodeId) const;

        // Returns node IDs in topological order (sources first, output last).
        // Throws std::logic_error if the graph contains a cycle.
        std::vector<uint32_t> TopologicalSort() const;

        // Ids of the nodes TopologicalSort cannot order: those on a cycle and
        // those downstream of one. Empty when the graph sorts.
        std::vector<uint32_t> UnorderableNodes() const;

        // Returns IDs of all Output nodes in the graph.
        std::vector<uint32_t> GetOutputNodeIds() const;

        // Returns true if a dependency from srcId to dstId would create a
        // cycle. Follows edges and bindings, as TopologicalSort does.
        bool WouldCreateCycle(uint32_t srcId, uint32_t dstId) const;

        // Calls visit(sourceId, dependentId) once per dependency of the kinds
        // asked for: image edges in edge order, then each node's distinct
        // binding sources. Every graph walk goes through it, so the sort, the
        // cycle check, dirty propagation and demand agree on what an edge is.
        void ForEachDependency(DependencyKind kinds,
            const std::function<void(uint32_t sourceId, uint32_t dependentId)>& visit) const;

        // Marks all nodes as dirty (forces full re-evaluation).
        void MarkAllDirty();

        // Marks every node reachable downstream of a dirty node, along image
        // edges, dirty too. Binding consumers are left to the evaluator: it
        // dirties them when a bound value actually changes, which a dirty
        // producer has not yet done.
        void PropagateDirtyDownstream();

        // Marks every node downstream of nodeId (image edges, not nodeId
        // itself) dirty.
        void MarkDownstreamDirty(uint32_t nodeId);

        // Returns true if any needed node has its dirty flag set.
        bool HasDirtyNodes() const;

        // Sets EffectNode::needed: true for every Output node, every dirty
        // Typed-analysis node, every node in extraRoots, and everything they
        // depend on through edges or bindings; false for the rest.
        void ComputeNeeded(std::span<const uint32_t> extraRoots);

        // Sets EffectNode::needed on every node, for hosts that may read any
        // node after a frame (headless MCP sessions, tests).
        void MarkAllNeeded();

        // Clears all cached outputs.
        void ClearCachedOutputs();

        // --- Property bindings ---

        // Bind a downstream node's property to an upstream analysis output field.
        // Validates type compatibility, rejects cycles, replaces existing binding.
        // Returns empty string on success, or an error message on failure.
        std::wstring BindProperty(
            uint32_t destNodeId,
            const std::wstring& propertyName,
            uint32_t sourceNodeId,
            const std::wstring& sourceFieldName,
            uint32_t sourceComponent = 0);

        // Remove a property binding. Returns true if a binding was removed.
        bool UnbindProperty(uint32_t nodeId, const std::wstring& propertyName);

        // Removes the components of a binding that read sourceFieldName on
        // sourceNodeId, and the whole binding when no component is left.
        // Returns false when the node has no binding on that property.
        bool UnbindPropertySource(uint32_t nodeId, const std::wstring& propertyName,
                                  uint32_t sourceNodeId, const std::wstring& sourceFieldName);

        // Returns true if the property type is bindable (float, float2, float3, float4).
        static bool IsBindablePropertyType(const PropertyValue& value);

        // --- Accessors ---

        const std::vector<EffectNode>& Nodes() const { return m_nodes; }
        // Mutable access to the nodes without the power to add or remove one.
        std::span<EffectNode> MutableNodes() { return m_nodes; }
        const std::vector<EffectEdge>& Edges() const { return m_edges; }
        bool IsEmpty() const { return m_nodes.empty(); }

        // --- Serialization (Windows.Data.Json) ---

        winrt::hstring ToJson() const;
        static GraphModel FromJson(winrt::hstring const& json);

        // --- Bulk operations ---

        void Clear();

    private:
        // Kahn's algorithm over ForEachDependency; stops short of the nodes
        // a cycle holds back.
        std::vector<uint32_t> KahnOrder() const;

        // Marks everything reachable from `from` along image edges dirty.
        void MarkReachableDirty(std::vector<uint32_t> from);

        std::vector<EffectNode> m_nodes;
        std::vector<EffectEdge> m_edges;
        uint32_t m_nextId{ 1 };
    };
}
