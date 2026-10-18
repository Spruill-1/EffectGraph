#pragma once

#include "pch_engine.h"
#include "../Graph/GraphModel.h"
#include "EvaluatorOptions.h"

#include <memory>
#include <unordered_set>

namespace ShaderLab::Effects
{
    class IEffectHooks;
}

namespace ShaderLab::Rendering
{
    class GpuTimer;

    namespace Evaluator
    {
        class EvaluatorCore;
    }

    // Evaluates an GraphModel by walking nodes in topological order,
    // creating / caching D2D effects, wiring inputs from upstream outputs,
    // and returning the final ID2D1Image* for presentation.
    //
    // The evaluator keeps a per-node effect cache so that effects are only
    // recreated when the node type or CLSID changes (not every frame).
    // Properties are re-applied each frame on dirty nodes.
    //
    // Hosts render frames through FrameExecutor, which orders the calls.
    //
    // The class holds only a pointer to its implementation
    // (Rendering/Evaluator/), so the exported layout does not change when
    // the evaluator's internals do.
    class GraphEvaluator
    {
    public:
        GraphEvaluator();
        ~GraphEvaluator();
        GraphEvaluator(const GraphEvaluator&) = delete;
        GraphEvaluator& operator=(const GraphEvaluator&) = delete;

        // Optional GPU timer, owned by the host. When set and enabled, the
        // evaluator brackets each compute dispatch so per-node GPU time can be
        // attributed exactly -- a compute dispatch is its own submission, so
        // this costs nothing and perturbs nothing. Image nodes are NOT timed
        // here: Direct2D evaluates an effect chain lazily at DrawImage, so
        // they are not separately dispatched and have to be materialised one
        // at a time, which is a profiling mode rather than free.
        void SetGpuTimer(GpuTimer* timer);

        // Pins this evaluator's options. Until then it follows the process
        // defaults (Performance::DefaultEvaluatorOptions). Callable from any
        // thread; the next Normal Evaluate picks the value up and holds it
        // for that frame.
        void SetOptions(const EvaluatorOptions& options);
        // Drops pinned options and follows the process defaults again.
        void FollowDefaultOptions();
        // The options the next frame will use.
        EvaluatorOptions Options() const;

        // Where built-in effects' C++ hooks (derived constants, image-output
        // sizing) come from. Defaults to the built-in catalog; nullptr runs
        // with no hooks. Not owned; must outlive the evaluator.
        void SetEffectHooks(const Effects::IEffectHooks* hooks);

        enum class EvalMode
        {
            Normal,
            // Re-applies properties and bindings only: queues no compute
            // dispatch, and leaves variant and async-readback polling to the
            // next Normal pass. FrameExecutor uses it for the sweep after
            // ProcessDeferredCompute.
            Frozen,
        };

        // Walk the graph in topological order and produce the final output image.
        // Returns nullptr if the graph is empty or has no Output node.
        ID2D1Image* Evaluate(Graph::GraphModel& graph, ID2D1DeviceContext5* dc,
                             EvalMode mode = EvalMode::Normal);

        // Run deferred D3D11 compute dispatches for statistics nodes.
        // Call AFTER Evaluate (and double-eval) when all D2D effects are
        // initialized, outside any draw session on `dc`: an input the shader
        // cannot read in place is rendered in a session of its own.
        // Returns true if any compute dispatches ran (analysis output changed).
        bool ProcessDeferredCompute(Graph::GraphModel& graph, ID2D1DeviceContext5* dc);
        // Compute dispatches Evaluate queued that ProcessDeferredCompute has
        // not run yet. ProcessDeferredCompute drains the queue, so this is 0
        // after a frame; DispatchesLastFrame() counts the work done.
        size_t DeferredComputeCount() const;

        // Compute dispatches actually issued during the last
        // ProcessDeferredCompute. Survives the drain.
        uint32_t DispatchesLastFrame() const;

        // Compile shaders on the cache's worker threads instead of the calling
        // thread (off by default). A node without its baseline shader has no
        // output, sets EffectNode::compilePending and stays dirty; a missing
        // variant falls back to the generic shader. The GUI turns it on so a
        // long compile does not stall its render thread; headless leaves it
        // off because a one-shot render must wait for the shader anyway.
        void SetAsyncCompile(bool on);

        // Async CPU readback (EvaluatorOptions::asyncAnalysisReadback, the
        // GUI's live loop): values copied for a node on one frame land on a
        // later Evaluate. True while any copy is still in flight, so a host
        // keeps rendering until the values have landed.
        bool HasPendingReadbacks() const;

        // Resolve property bindings for Source nodes only (lightweight, no D2D).
        // Call before TickAndUploadVideos so video nodes see updated Time values.
        void ResolveSourceBindings(Graph::GraphModel& graph);

        // Release all cached D2D effects (e.g., on device lost or graph clear).
        // The graph-aware overload also clears every node's non-owning cachedOutput
        // pointer so the render path can't dereference a freed effect.
        void ReleaseCache();
        void ReleaseCache(Graph::GraphModel& graph);

        // Drops everything kept for one node's effect instance (e.g., CLSID
        // changed). The graph-aware overload also clears node.cachedOutput.
        void InvalidateNode(uint32_t nodeId);
        void InvalidateNode(Graph::GraphModel& graph, uint32_t nodeId);

        // Update an existing cached effect's shader bytecode in-place (for recompile).
        // If the effect isn't cached yet, does nothing (next Evaluate will create it).
        void UpdateNodeShader(uint32_t nodeId, const Graph::EffectNode& node);

        // The GUID a shader is loaded into D2D under: the definition's GUID
        // mixed with the bytecode's identity. D2D ignores LoadPixelShader for
        // a GUID it already holds, so different bytecode needs a different GUID.
        static GUID ShaderGuidFor(const GUID& definitionGuid, const std::vector<uint8_t>& bytecode);

        // Clean-subgraph caching telemetry (D2D1_PROPERTY_CACHED).
        size_t   CachedEffectCount() const;
        uint64_t CacheInvalidations() const;
        // Evaluate passes in which this node's output changed (it re-rendered,
        // or its analysis fields moved). A host compares it across a frame to
        // learn whether the node rendered in that frame.
        uint64_t OutputChangeCount(uint32_t nodeId) const;

        // Host hint: nodes whose analysis values must reach the CPU (the
        // selected node, so its Properties panel stays live, plus any node an
        // MCP route is about to read). Consulted only when the skip-readback
        // option is on; with it off every compute dispatch reads back.
        //
        // Throttling: a node newly added to the set reads back at once, then
        // at most every EvaluatorOptions::cpuAnalysisHintThrottleMs. A node
        // that leaves the set and is re-added counts as new.
        void SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids);
        void ClearCpuAnalysisInterest();

        // Reads the node's analysis values back to the CPU, blocking, on its
        // next dispatch, whatever the skip, throttle and async settings say.
        // One-shot: the request is spent by that dispatch. A caller that needs
        // the values now also dirties the node and renders a frame. Only a
        // D3D11 compute node spends the request; one for another node stays
        // until ReleaseCache or the node leaves the graph. Call on the thread
        // that runs Evaluate.
        void RequestFreshCpuReadback(uint32_t nodeId);

        // GPU-binding bits of the shader build the node last ran: two bits per
        // gpuBindable parameter, 1 = structured buffer, 2 = analysis texture,
        // 0 = cbuffer. 0 for a node with no GPU-served binding.
        uint32_t AppliedGpuBits(uint32_t nodeId) const;

        // A D3D11 compute node's analysis results on the GPU, as the
        // StructuredBuffer<float4> its last dispatch wrote; null for any
        // other node or before its first dispatch.
        winrt::com_ptr<ID3D11ShaderResourceView> AnalysisBuffer(uint32_t nodeId) const;
        // Overwrites elements of that buffer until the node next dispatches,
        // refreshing the copy pixel-shader consumers read.
        HRESULT WriteAnalysis(uint32_t nodeId, uint32_t firstElement, const float* float4s, uint32_t elementCount);

    private:
        std::unique_ptr<Evaluator::EvaluatorCore> m_core;
    };
}
