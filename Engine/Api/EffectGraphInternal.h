#pragma once

#include "pch_engine.h"
#include "EffectGraph.h"

namespace ShaderLab::Graph { class GraphModel; }
namespace ShaderLab::Effects { class SourceNodeFactory; }
namespace ShaderLab::Rendering
{
    class GraphEvaluator;
    struct DeviceStack;
    struct FrameRequest;
    struct FrameResult;
    struct DisplayProfile;
}

// The context as ShaderLab's own hosts and tests see it: the C++ objects behind
// the public interfaces, for what the public API leaves out on purpose (canvas
// layout, the UI snapshot, Effect Designer compiles, perf counters, MCP).
// Reached with QueryInterface from IEffectGraphContext, by programs that link
// the engine library (ShaderLab's hosts and tests) and only on contexts they
// created themselves. It is not part of EffectGraph.dll's contract: the DLL's
// users never see this header, and its layout follows the engine's sources.

MIDL_INTERFACE("684bd71b-4d22-4ce7-b0d9-a5bf835e25c3")
IEffectGraphContextInternal : public IEffectGraphContext
{
    // The graph, evaluator, sources and devices the context owns. Same
    // threading rule as the context: one thread at a time.
    STDMETHOD_(ShaderLab::Graph::GraphModel&, Model)() PURE;
    STDMETHOD_(ShaderLab::Rendering::GraphEvaluator&, Evaluator)() PURE;
    STDMETHOD_(ShaderLab::Effects::SourceNodeFactory&, Sources)() PURE;
    STDMETHOD_(const ShaderLab::Rendering::DeviceStack&, Devices)() PURE;
    // The context's own D2D device context, without a reference added. Only
    // ShaderLab's hosts draw on it, never while an evaluation is running.
    STDMETHOD_(ID2D1DeviceContext5*, DeviceContext)() PURE;

    // The graph id behind a node interface; 0 when it was removed or belongs
    // to another context.
    STDMETHOD_(uint32_t, NodeId)(IEffectGraphNode* node) PURE;

    // The node interface for a graph id: the same object while anything holds
    // a reference to it. S_FALSE and null for a missing id.
    STDMETHOD(NodeFor)(uint32_t nodeId, IEffectGraphNode** node) PURE;

    // One frame as a host drives it: its own demand roots, target, GPU timer
    // and draw callback (Rendering::FrameExecutor::Run on the context's
    // device context).
    STDMETHOD_(ShaderLab::Rendering::FrameResult, RunFrame)(const ShaderLab::Rendering::FrameRequest& request) PURE;

    // The display Working Space nodes describe before every frame: a host's
    // monitor or pinned profile. The context uses the defaults until set.
    STDMETHOD_(void, SetDisplayProfile)(const ShaderLab::Rendering::DisplayProfile& profile) PURE;

    // Replaces the whole graph: a host's File > Open or clear, or MCP's
    // /graph/load. Everything from the old graph goes (cached effects, input
    // images), node interfaces from before see their node as removed, and
    // the new graph's sources are opened. Media a host extracted for `graph`
    // stays the host's to delete.
    STDMETHOD_(void, ReplaceGraph)(ShaderLab::Graph::GraphModel&& graph) PURE;
};
