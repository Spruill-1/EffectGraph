#include "pch_engine.h"
#include "GraphEvaluator.h"
#include "Evaluator/EvaluatorCore.h"

// The exported facade: every call forwards to Evaluator::EvaluatorCore.
namespace ShaderLab::Rendering
{
    GraphEvaluator::GraphEvaluator() : m_core(std::make_unique<Evaluator::EvaluatorCore>()) {}
    GraphEvaluator::~GraphEvaluator() = default;

    void GraphEvaluator::SetGpuTimer(GpuTimer* timer) { m_core->SetGpuTimer(timer); }
    void GraphEvaluator::SetOptions(const EvaluatorOptions& options) { m_core->SetOptions(options); }
    void GraphEvaluator::FollowDefaultOptions() { m_core->FollowDefaultOptions(); }
    EvaluatorOptions GraphEvaluator::Options() const { return m_core->Options(); }
    void GraphEvaluator::SetEffectHooks(const Effects::IEffectHooks* hooks) { m_core->SetEffectHooks(hooks); }

    ID2D1Image* GraphEvaluator::Evaluate(Graph::GraphModel& graph, ID2D1DeviceContext5* dc, EvalMode mode)
    {
        return m_core->Evaluate(graph, dc, mode);
    }

    bool GraphEvaluator::ProcessDeferredCompute(Graph::GraphModel& graph, ID2D1DeviceContext5* dc)
    {
        return m_core->ProcessDeferredCompute(graph, dc);
    }

    size_t GraphEvaluator::DeferredComputeCount() const { return m_core->DeferredComputeCount(); }
    uint32_t GraphEvaluator::DispatchesLastFrame() const { return m_core->DispatchesLastFrame(); }
    void GraphEvaluator::SetAsyncCompile(bool on) { m_core->SetAsyncCompile(on); }
    bool GraphEvaluator::HasPendingReadbacks() const { return m_core->HasPendingReadbacks(); }
    void GraphEvaluator::ResolveSourceBindings(Graph::GraphModel& graph) { m_core->ResolveSourceBindings(graph); }

    void GraphEvaluator::ReleaseCache() { m_core->ReleaseCache(); }
    void GraphEvaluator::ReleaseCache(Graph::GraphModel& graph) { m_core->ReleaseCache(graph); }
    void GraphEvaluator::InvalidateNode(uint32_t nodeId) { m_core->InvalidateNode(nodeId); }
    void GraphEvaluator::InvalidateNode(Graph::GraphModel& graph, uint32_t nodeId) { m_core->InvalidateNode(graph, nodeId); }
    void GraphEvaluator::UpdateNodeShader(uint32_t nodeId, const Graph::EffectNode& node) { m_core->UpdateNodeShader(nodeId, node); }

    GUID GraphEvaluator::ShaderGuidFor(const GUID& definitionGuid, const std::vector<uint8_t>& bytecode)
    {
        static_assert(sizeof(GUID) == sizeof(Evaluator::BytecodeId));
        const Evaluator::BytecodeId id = Evaluator::IdOfBytecode(bytecode);
        GUID guid = definitionGuid;
        auto* guidBytes = reinterpret_cast<uint8_t*>(&guid);
        for (size_t index = 0; index < id.size(); ++index)
            guidBytes[index] ^= id[index];
        return guid;
    }

    size_t GraphEvaluator::CachedEffectCount() const { return m_core->CachedEffectCount(); }
    uint64_t GraphEvaluator::CacheInvalidations() const { return m_core->CacheInvalidations(); }
    uint64_t GraphEvaluator::OutputChangeCount(uint32_t nodeId) const { return m_core->OutputChangeCount(nodeId); }

    void GraphEvaluator::SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids)
    {
        m_core->SetCpuAnalysisInterest(std::move(ids));
    }
    void GraphEvaluator::ClearCpuAnalysisInterest() { m_core->ClearCpuAnalysisInterest(); }
    void GraphEvaluator::RequestFreshCpuReadback(uint32_t nodeId) { m_core->RequestFreshCpuReadback(nodeId); }
    uint32_t GraphEvaluator::AppliedGpuBits(uint32_t nodeId) const { return m_core->AppliedGpuBits(nodeId); }
    winrt::com_ptr<ID3D11ShaderResourceView> GraphEvaluator::AnalysisBuffer(uint32_t nodeId) const { return m_core->AnalysisBuffer(nodeId); }
    HRESULT GraphEvaluator::WriteAnalysis(uint32_t nodeId, uint32_t firstElement, const float* float4s, uint32_t elementCount)
    {
        return m_core->WriteAnalysis(nodeId, firstElement, float4s, elementCount);
    }
}
