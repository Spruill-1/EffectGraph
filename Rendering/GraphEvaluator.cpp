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
    Graph::DirtyRegion GraphEvaluator::PendingRegion(uint32_t nodeId) const { return m_core->PendingRegion(nodeId); }
    void GraphEvaluator::AcknowledgeRegion(uint32_t nodeId) { m_core->AcknowledgeRegion(nodeId); }
    const AnalysisReadbackStats& GraphEvaluator::ReadbackStats() const { return m_core->ReadbackStats(); }
    const BindingGateStats& GraphEvaluator::BindingGate() const { return m_core->BindingGate(); }
    const ReduceCellStats& GraphEvaluator::ReduceCells() const { return m_core->ReduceCells(); }
    NodeReduceCellStats GraphEvaluator::ReduceCells(uint32_t nodeId) const { return m_core->ReduceCells(nodeId); }
    const PreRenderStats& GraphEvaluator::PreRenders() const { return m_core->PreRenders(); }
    Graph::DirtyRegion GraphEvaluator::PreRenderPending(uint32_t producerId, uint32_t outputPin) const
    {
        return m_core->PreRenderPending(producerId, outputPin);
    }
    FallbackBuildsInUse GraphEvaluator::FallbacksInUse(const Graph::GraphModel& graph) const { return m_core->FallbacksInUse(graph); }
    Graph::DirtyRegion GraphEvaluator::DispatchInputRegion(uint32_t nodeId) const { return m_core->DispatchInputRegion(nodeId); }
    void GraphEvaluator::FailNextComputeDispatchForTest(bool fail) { m_core->m_failNextComputeDispatch = fail; }
    bool GraphEvaluator::FailNextComputeDispatchArmed() const { return m_core->m_failNextComputeDispatch; }
    void GraphEvaluator::FailNextPreRenderDrawForTest(bool fail) { m_core->m_failNextPreRenderDraw = fail; }
    bool GraphEvaluator::FailNextPreRenderDrawArmed() const { return m_core->m_failNextPreRenderDraw; }
    void GraphEvaluator::SetUncachedNodes(std::unordered_set<uint32_t> ids) { m_core->SetUncachedNodes(std::move(ids)); }
    const RegionDrawStats& GraphEvaluator::RegionStats() const { return m_core->RegionStats(); }
    RegionDrawStats& GraphEvaluator::MutableRegionStats() { return m_core->RegionStats(); }

    void GraphEvaluator::SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids)
    {
        m_core->SetCpuAnalysisInterest(std::move(ids));
    }
    void GraphEvaluator::ClearCpuAnalysisInterest() { m_core->ClearCpuAnalysisInterest(); }
    void GraphEvaluator::RequestFreshCpuReadback(uint32_t nodeId) { m_core->RequestFreshCpuReadback(nodeId); }
    uint32_t GraphEvaluator::AppliedGpuBits(uint32_t nodeId) const { return m_core->AppliedGpuBits(nodeId); }
    winrt::com_ptr<ID3D11ShaderResourceView> GraphEvaluator::AnalysisBuffer(uint32_t nodeId) const { return m_core->AnalysisBuffer(nodeId); }
    HRESULT GraphEvaluator::WriteAnalysis(uint32_t nodeId, uint32_t firstElement, const float* float4s, uint32_t elementCount,
                                          const Graph::GraphModel* graph)
    {
        return m_core->WriteAnalysis(graph, nodeId, firstElement, float4s, elementCount);
    }
    void GraphEvaluator::FailNextTextureCopyForTest(uint32_t nodeId) { m_core->FailNextTextureCopyForTest(nodeId); }
    void GraphEvaluator::FailNextReadbackPollForTest(uint32_t nodeId) { m_core->FailNextReadbackPollForTest(nodeId); }
    void GraphEvaluator::FailNextComputeSubmitForTest(uint32_t nodeId) { m_core->FailNextComputeSubmitForTest(nodeId); }

    bool GraphEvaluator::PollReadbacks(Graph::GraphModel& graph, bool block) { return m_core->PollReadbacks(graph, block); }
    bool GraphEvaluator::LandedValues(uint32_t nodeId, std::vector<Graph::AnalysisFieldValue>& values) const
    {
        return m_core->LandedValues(nodeId, values);
    }
    bool GraphEvaluator::CommittedValues(const Graph::GraphModel& graph, uint32_t nodeId,
                                         std::vector<Graph::AnalysisFieldValue>& values, bool& pending) const
    {
        return m_core->CommittedValues(graph, nodeId, values, pending);
    }
    HRESULT GraphEvaluator::CommitFields(const Graph::GraphModel& graph, uint32_t nodeId, const uint32_t* fieldIndices,
                                         const float* values, uint32_t count)
    {
        return m_core->CommitFields(graph, nodeId, fieldIndices, values, count);
    }
    std::optional<Graph::CommitPolicy> GraphEvaluator::FieldCommitPolicy(const Graph::GraphModel& graph, uint32_t nodeId,
                                                                         uint32_t fieldIndex) const
    {
        return m_core->FieldCommitPolicy(graph, nodeId, fieldIndex);
    }
    const DeferredCommitStats& GraphEvaluator::DeferredCommit() const { return m_core->DeferredStats(); }
    void GraphEvaluator::ResetDeferredCommitStats() { m_core->ResetDeferredStats(); }
    HRESULT GraphEvaluator::ReadAnalysisTexture(uint32_t nodeId, std::vector<float>& float4s) const
    {
        return m_core->ReadAnalysisTexture(nodeId, float4s);
    }
}
