#pragma once

#include "pch_engine.h"
#include "../Graph/GraphModel.h"
#include "GraphEvaluator.h"

namespace ShaderLab::Rendering
{
    class GraphEvaluator;
    class GpuTimer;

    // Which nodes a frame evaluates (EffectNode::needed).
    enum class FrameDemand
    {
        // Every node: for hosts that may read any node afterwards (a headless
        // MCP session, tests).
        All,
        // Output nodes, dirty Typed-analysis nodes, FrameRequest::roots, and
        // everything they depend on (GraphModel::ComputeNeeded).
        Roots,
    };

    // Whether a second Evaluate pass runs before the draw session.
    enum class SecondPass
    {
        // When the first pass left needed nodes dirty: new custom effects need
        // two passes before their output is valid.
        IfDirty,
        // Never: for a caller that already settled every effect and re-dirties
        // the whole graph per frame, where a second pass only re-queues work.
        Never,
    };

    struct FrameRequest
    {
        // Bound for `draw`; may be null.
        ID2D1Image* target{ nullptr };
        bool markAllDirty{ false };
        FrameDemand demand{ FrameDemand::All };
        std::vector<uint32_t> roots;
        SecondPass secondPass{ SecondPass::IfDirty };
        // Nodes whose analysis values the host wants on the CPU this frame
        // (GraphEvaluator::SetCpuAnalysisInterest); unset leaves it as is.
        std::optional<std::unordered_set<uint32_t>> analysisInterest;
        // Brackets the Evaluate, DeferredCompute and Draw spans when set. The
        // host owns BeginFrame/EndFrame and the Frame span.
        GpuTimer* timer{ nullptr };
        // Runs after compute in a draw session of its own, with `target`
        // bound. With no draw the frame opens no session for it.
        std::function<void(ID2D1DeviceContext5*)> draw;
        // Roots `draw` draws every pending rect of (GraphEvaluator::
        // PendingRegion). Each one's pending region is cleared when EndDraw
        // succeeds and the root has an output, no compile pending and no
        // error; otherwise it stays pending for the next draw.
        std::vector<uint32_t> acknowledgeRoots;
        // Nodes whose D2D output is not cached this frame (GraphEvaluator::
        // SetUncachedNodes): roots drawn incrementally, so a clipped draw and
        // a whole one round the same way.
        std::vector<uint32_t> uncachedRoots;
        // A frame in which any node chose or still runs a build that serves a
        // GPU binding from the CPU (AnalysisReadbackStats::fallbackBuilds or
        // fallbackBuildsPending moved, or GraphEvaluator::FallbacksInUse is
        // non-zero) draws nothing and acknowledges no root:
        // EFFECTGRAPH_CONTEXT_FLAGS_NO_FALLBACK.
        bool refuseFallbackBuilds{ false };
    };

    struct FrameResult
    {
        // The draw's EndDraw; S_OK when the frame drew nothing. A compute
        // input that failed to render fails its node instead.
        HRESULT endDraw{ S_OK };
        // The device's removed reason after the frame, when it was removed
        // (DXGI_ERROR_DEVICE_REMOVED, ...); S_OK otherwise. Set by
        // IEffectGraphContextInternal::RunFrame.
        HRESULT deviceRemoved{ S_OK };
        // Evaluate passes, the frozen post-dispatch sweep included.
        uint32_t passes{ 0 };
        // ProcessDeferredCompute ran a non-empty queue.
        bool computeRan{ false };
        // Compute dispatches issued this frame.
        uint32_t dispatches{ 0 };
        // Builds chosen this frame without the GPU binding the plan wanted:
        // missing, or still compiling.
        uint32_t fallbackBuilds{ 0 };
        uint32_t fallbackBuildsPending{ 0 };
        // Needed nodes running a fallback build after the passes, whenever chosen.
        FallbackBuildsInUse fallbacksInUse;
        // The draw did not run: FrameRequest::refuseFallbackBuilds found one.
        bool fallbackRefused{ false };
        // Phase boundaries: drawEnd is before EndDraw, end after it.
        std::chrono::steady_clock::time_point start, evaluateEnd, computeEnd, drawEnd, end;
    };

    // Runs one frame of a graph through the evaluator: dirty propagation,
    // demand, Evaluate (twice when new effects need it), the deferred compute
    // dispatches, a frozen Evaluate when anything needed is still dirty (to
    // re-apply the D2D nodes downstream of this frame's dispatches without
    // queueing compute again), then the host's draw. No step runs inside
    // another's draw session: each opens one only for its own drawing (an
    // analysis readback, a compute input the shader cannot read in place, the
    // draw), so a graph of compute on texture inputs opens none. Call it with
    // no session open on `dc`. The context's target and transform are
    // restored on return.
    class FrameExecutor
    {
    public:
        static FrameResult Run(Graph::GraphModel& graph, GraphEvaluator& evaluator,
                               ID2D1DeviceContext5* dc, const FrameRequest& request);
    };
}
