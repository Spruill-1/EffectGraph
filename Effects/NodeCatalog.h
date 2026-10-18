#pragma once

#include "pch_engine.h"
#include "../Graph/EffectNode.h"

namespace ShaderLab::Graph
{
    class GraphModel;
}

// Creating a node from the name a client gives it, the one lookup behind
// /graph/add-node and /graph/apply.
namespace ShaderLab::Effects
{
    // What a node is created with besides its name.
    struct NodeCreateArgs
    {
        std::wstring filePath;   // Video / Image Source
        // Named arguments a host's node maker reads ("adapterIndex").
        std::map<std::wstring, uint32_t, std::less<>> uints;
    };

    struct NodeCreateResult
    {
        std::optional<Graph::EffectNode> node;
        std::wstring error;           // why there is no node
        // A Source that needs SourceNodeFactory::PrepareSourceNode once added:
        // a file-backed Video / Image Source, or a live source.
        bool needsPrepare{ false };
    };

    // Makes a node a host adds to the catalog: a node of its own live source
    // kind, say. Returns a result with a node or an error.
    using NodeMaker = std::function<NodeCreateResult(const NodeCreateArgs& args, const Graph::GraphModel& graph)>;

    // Adds `maker` under each of `names`, process-wide. A name that is also
    // a built-in effect or node type still makes the built-in. Call at
    // startup, before any lookup.
    void RegisterNodeMaker(std::vector<std::wstring> names, NodeMaker maker);

    // Resolves, in order: "Custom Pixel Shader" / "Custom Compute Shader" /
    // "Custom D3D11 Compute Shader" (an empty shader), a ShaderLab effect, a
    // Direct2D built-in, "Output" (numbered "Output 2", ... after the first in
    // `graph`), "Video Source" / "Video" / "Image Source" / "Image", and the
    // names given to RegisterNodeMaker.
    NodeCreateResult CreateNodeByName(
        std::wstring_view name, const NodeCreateArgs& args, const Graph::GraphModel& graph);

    // A node running an authored custom effect: one input pin per input name,
    // an output pin and the custom effect's CLSID unless it is a D3D11 compute
    // (data only), each parameter at its default, and contracts derived from
    // those properties as a loaded graph's would be.
    Graph::EffectNode CreateCustomEffectNode(Graph::CustomEffectDefinition definition, std::wstring name);
}
