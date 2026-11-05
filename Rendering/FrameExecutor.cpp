#include "pch_engine.h"
#include "FrameExecutor.h"
#include "GraphEvaluator.h"
#include "GpuTimer.h"

namespace ShaderLab::Rendering
{
    FrameResult FrameExecutor::Run(Graph::GraphModel& graph, GraphEvaluator& evaluator,
                                   ID2D1DeviceContext5* dc, const FrameRequest& request)
    {
        FrameResult result;
        result.start = std::chrono::steady_clock::now();
        if (!dc) return result;

        // The context may be shared with the host: give back its target and
        // transform on every exit.
        struct RestoreContext
        {
            ID2D1DeviceContext5* dc;
            winrt::com_ptr<ID2D1Image> target;
            D2D1_MATRIX_3X2_F transform{};
            ~RestoreContext()
            {
                dc->SetTarget(target.get());
                dc->SetTransform(transform);
            }
        } restore{ dc };
        dc->GetTarget(restore.target.put());
        dc->GetTransform(&restore.transform);

        GpuTimer* timer = request.timer;

        if (request.markAllDirty)
            graph.MarkAllDirty();
        graph.PropagateDirtyDownstream(evaluator.Options().dirtyRegions);
        if (request.demand == FrameDemand::All)
            graph.MarkAllNeeded();
        else
            graph.ComputeNeeded(request.roots);

        evaluator.SetUncachedNodes({ request.uncachedRoots.begin(), request.uncachedRoots.end() });
        const AnalysisReadbackStats readbacksBefore = evaluator.ReadbackStats();
        if (timer) timer->Begin(GpuSpan::Evaluate);
        dc->SetTarget(nullptr);
        evaluator.Evaluate(graph, dc);
        ++result.passes;
        if (request.secondPass == SecondPass::IfDirty && graph.HasDirtyNodes())
        {
            evaluator.Evaluate(graph, dc);
            ++result.passes;
        }
        if (timer) timer->End(GpuSpan::Evaluate);
        result.evaluateEnd = std::chrono::steady_clock::now();

        if (timer) timer->Begin(GpuSpan::DeferredCompute);
        if (request.analysisInterest)
            evaluator.SetCpuAnalysisInterest(*request.analysisInterest);
        result.computeRan = evaluator.ProcessDeferredCompute(graph, dc);
        // DispatchesLastFrame is 0 on a frame that dispatched nothing.
        result.dispatches = result.computeRan ? evaluator.DispatchesLastFrame() : 0;
        if (graph.HasDirtyNodes())
        {
            evaluator.Evaluate(graph, dc, GraphEvaluator::EvalMode::Frozen);
            ++result.passes;
        }
        if (timer) timer->End(GpuSpan::DeferredCompute);
        result.computeEnd = std::chrono::steady_clock::now();

        const auto& readbacks = evaluator.ReadbackStats();
        result.fallbackBuilds = static_cast<uint32_t>(readbacks.fallbackBuilds - readbacksBefore.fallbackBuilds);
        result.fallbackBuildsPending =
            static_cast<uint32_t>(readbacks.fallbackBuildsPending - readbacksBefore.fallbackBuildsPending);
        // A node chose its fallback this frame, or is still running one it chose
        // earlier: a clean node is not re-selected, so the counters alone miss it.
        result.fallbacksInUse = evaluator.FallbacksInUse(graph);
        result.fallbackRefused = request.refuseFallbackBuilds &&
            (result.fallbackBuilds || result.fallbackBuildsPending ||
             result.fallbacksInUse.missing || result.fallbacksInUse.pending);

        if (timer) timer->Begin(GpuSpan::Draw);
        result.drawEnd = result.computeEnd;
        if (request.draw && !result.fallbackRefused)
        {
            dc->SetTarget(request.target);
            dc->BeginDraw();
            request.draw(dc);
            result.drawEnd = std::chrono::steady_clock::now();
            result.endDraw = dc->EndDraw();
            if (SUCCEEDED(result.endDraw))
                for (uint32_t id : request.acknowledgeRoots)
                    if (const auto* root = graph.FindNode(id);
                        root && root->cachedOutput && !root->compilePending && root->runtimeError.empty())
                        evaluator.AcknowledgeRegion(id);
        }
        result.end = std::chrono::steady_clock::now();
        if (timer) timer->End(GpuSpan::Draw);
        return result;
    }
}
