#pragma once

#include "pch_engine.h"
#include "../Graph/GraphModel.h"

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
