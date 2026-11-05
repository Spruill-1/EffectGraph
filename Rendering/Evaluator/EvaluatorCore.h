#pragma once

#include "pch_engine.h"
#include "../GraphEvaluator.h"
#include "../GpuBindingPlan.h"
#include "../GpuTimer.h"
#include "../../Effects/ShaderLabEffects.h"
#include "BindingResolver.h"
#include "NodeRuntime.h"
#include "VariantSelector.h"

// GraphEvaluator's implementation. The class is split across files by seam:
//
//   EvaluatorCore.cpp     the Evaluate walk and the simple node kinds
//   CustomEffectNode.cpp  custom D2D pixel / compute shader nodes
//   ComputeScheduler.cpp  D3D11 compute: queueing, ProcessDeferredCompute
//   EffectFactory.cpp     effect creation, property application, input wiring
//   AnalysisReadback.cpp  CPU readback of analysis values, lane-3 textures
//   CachePolicy.cpp       D2D output caching and invalidation
namespace ShaderLab::Rendering::Evaluator
{
    using EvalMode = GraphEvaluator::EvalMode;

    // Prefix of the runtimeError a failed effect creation leaves, so a later
    // success can clear exactly that error.
    inline constexpr std::wstring_view cCreateEffectFailedPrefix = L"Failed to create D2D effect: ";
    inline constexpr std::wstring_view cCommitPolicyConflictPrefix = L"Commit policy conflict: ";

    // A custom pixel shader's fixed output size, or nullopt to let D2D size
    // it from its inputs. A generator (no inputs) without one is 512 x 512.
    std::optional<D2D1_SIZE_U> FixedPixelOutputSize(const Graph::CustomEffectDefinition& def,
                                                    const PropertyMap& props);

    class EvaluatorCore
    {
    public:
        EvaluatorCore();

        // ---- GraphEvaluator's public surface (see GraphEvaluator.h) -------
        void SetGpuTimer(GpuTimer* timer) { m_gpuTimer = timer; }
        void SetOptions(const EvaluatorOptions& options);
        void FollowDefaultOptions();
        EvaluatorOptions Options() const;
        void SetEffectHooks(const Effects::IEffectHooks* hooks) { m_effectHooks = hooks; }
        void SetAsyncCompile(bool on) { m_variants.SetAsyncCompile(on); }

        ID2D1Image* Evaluate(Graph::GraphModel& graph, ID2D1DeviceContext5* dc, EvalMode mode);
        bool ProcessDeferredCompute(Graph::GraphModel& graph, ID2D1DeviceContext5* dc);
        size_t DeferredComputeCount() const { return m_computeQueue.entries.size(); }
        uint32_t DispatchesLastFrame() const { return m_dispatchesLastFrame; }

        // Collects async readbacks copied on an earlier frame. True when any
        // node's analysis fields changed; their bound consumers are marked
        // dirty. Evaluate calls it at the start of each Normal pass.
        bool PollAsyncReadbacks(Graph::GraphModel& graph);
        bool HasPendingReadbacks() const;
        void ResolveSourceBindings(Graph::GraphModel& graph);

        void ReleaseCache();
        void ReleaseCache(Graph::GraphModel& graph);
        void InvalidateNode(uint32_t nodeId);
        void InvalidateNode(Graph::GraphModel& graph, uint32_t nodeId);
        void UpdateNodeShader(uint32_t nodeId, const Graph::EffectNode& node);

        size_t CachedEffectCount() const;
        uint64_t CacheInvalidations() const { return m_cacheInvalidations; }
        uint64_t OutputChangeCount(uint32_t nodeId) const;
        Graph::DirtyRegion PendingRegion(uint32_t nodeId) const;
        void AcknowledgeRegion(uint32_t nodeId);
        RegionDrawStats& RegionStats() { return m_regionStats; }
        const RegionDrawStats& RegionStats() const { return m_regionStats; }
        void SetUncachedNodes(std::unordered_set<uint32_t> ids) { m_uncachedNodes = std::move(ids); }
        bool CachesOutput(uint32_t nodeId) const { return !m_uncachedNodes.contains(nodeId); }
        const AnalysisReadbackStats& ReadbackStats() const { return m_readbackStats; }
        const BindingGateStats& BindingGate() const { return m_bindingGate; }
        const ReduceCellStats& ReduceCells() const { return m_reduceCells; }
        Graph::DirtyRegion DispatchInputRegion(uint32_t nodeId) const;
        NodeReduceCellStats ReduceCells(uint32_t nodeId) const;
        const PreRenderStats& PreRenders() const { return m_preRenders; }
        Graph::DirtyRegion PreRenderPending(uint32_t producerId, uint32_t outputPin) const;
        // GraphEvaluator::FailNextComputeDispatchForTest.
        bool m_failNextComputeDispatch{ false };
        // GraphEvaluator::FailNextPreRenderDrawForTest.
        bool m_failNextPreRenderDraw{ false };
        FallbackBuildsInUse FallbacksInUse(const Graph::GraphModel& graph) const;

        void SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids);
        void ClearCpuAnalysisInterest();
        void RequestFreshCpuReadback(uint32_t nodeId) { m_nodes[nodeId].freshReadbackRequested = true; }
        uint32_t AppliedGpuBits(uint32_t nodeId) const;
        winrt::com_ptr<ID3D11ShaderResourceView> AnalysisBuffer(uint32_t nodeId) const;
        HRESULT WriteAnalysis(const Graph::GraphModel* graph, uint32_t nodeId, uint32_t firstElement,
                              const float* float4s, uint32_t elementCount);
        void FailNextTextureCopyForTest(uint32_t nodeId);
        void FailNextReadbackPollForTest(uint32_t nodeId);
        void FailNextComputeSubmitForTest(uint32_t nodeId);

        // ---- deferred analysis commit (DeferredCommit.cpp) ---------------
        bool PollReadbacks(Graph::GraphModel& graph, bool block);
        bool LandedValues(uint32_t nodeId, std::vector<Graph::AnalysisFieldValue>& values) const;
        bool CommittedValues(const Graph::GraphModel& graph, uint32_t nodeId, std::vector<Graph::AnalysisFieldValue>& values,
                             bool& pending) const;
        HRESULT CommitFields(const Graph::GraphModel& graph, uint32_t nodeId, const uint32_t* fieldIndices,
                             const float* values, uint32_t count);
        std::optional<Graph::CommitPolicy> FieldCommitPolicy(const Graph::GraphModel& graph, uint32_t nodeId,
                                                             uint32_t fieldIndex) const;
        const DeferredCommitStats& DeferredStats() const { return m_deferredStats; }
        void ResetDeferredStats() { m_deferredStats = {}; }
        HRESULT ReadAnalysisTexture(uint32_t nodeId, std::vector<float>& float4s) const;

    private:
        // ---- deferred analysis commit (DeferredCommit.cpp) ---------------
        // The policy of each of a producer's fields, from the bindings that
        // read it. Unbound fields are Exact; with the commit off, Immediate.
        struct FieldPolicies
        {
            std::vector<Graph::CommitPolicy> policy;
            std::vector<uint8_t> bound;
            // Bindings disagree on the field's policy; it commits as Exact.
            std::vector<uint8_t> conflicted;
            bool Any(Graph::CommitPolicy p) const
            {
                for (size_t f = 0; f < policy.size(); ++f)
                    if (bound[f] && policy[f] == p) return true;
                return false;
            }
            // Some bound field commits later than its dispatch.
            bool AnyDeferred() const
            {
                return Any(Graph::CommitPolicy::Exact) || Any(Graph::CommitPolicy::Threshold) ||
                       Any(Graph::CommitPolicy::Hosted);
            }
        };
        // The commit is on for this frame: it needs the skip-readback option.
        bool DeferredCommitOn() const
        {
            return m_frameOptions.deferredAnalysisCommit && m_frameOptions.skipUnneededCpuReadback;
        }
        FieldPolicies PoliciesOf(const Graph::GraphModel& graph, const Graph::EffectNode& producer) const;
        // The same, computed once per producer between graph edits: cleared
        // where a pass, the compute drain or a poll starts.
        const FieldPolicies& CachedPoliciesOf(const Graph::GraphModel& graph, const Graph::EffectNode& producer) const;
        mutable std::unordered_map<uint32_t, FieldPolicies> m_policyCache;
        // Why a consumer's bindings cannot resolve: two of them disagree on
        // a field's policy. Empty when they agree, or the commit is off.
        std::wstring CommitPolicyConflict(const Graph::EffectNode& consumer, const Graph::GraphModel& graph) const;
        // Before a deferred producer's dispatch: what its lane-3 copy pass and
        // a WriteResults copy write. `wholeInput`: the dispatch is not over rects.
        void PrepareDeferredDispatch(const Graph::EffectNode& node, NodeRuntime& runtime,
                                     Effects::CustomComputeBridgeEffect* bridge, const FieldPolicies& policies,
                                     bool wholeInput);
        // After it: stamps the record, wakes the consumers whose texels the
        // copy pass wrote, and records the readback in flight or lands the
        // blocking one (`blockingFloats`).
        void FinishDeferredDispatch(Graph::GraphModel& graph, Graph::EffectNode& node, NodeRuntime& runtime,
                                    Effects::CustomComputeBridgeEffect* bridge, const FieldPolicies& policies,
                                    bool wholeInput, bool filledBefore, uint64_t generationBefore,
                                    uint64_t seqBefore, const std::vector<float>* blockingFloats);
        // A readback of `slot`'s dispatch reached the CPU: compare, commit
        // and wake, under each field's policy. True when a field committed.
        bool LandDeferred(Graph::GraphModel& graph, Graph::EffectNode& node, NodeRuntime& runtime,
                          const DeferredCommit::InFlight& slot, const std::vector<float>& floats);
        // WriteAnalysis under a deferred commit: the buffer takes the values;
        // the texture and the record only the written fields that are not Hosted.
        HRESULT WriteDeferredResults(const Graph::GraphModel& graph, const Graph::EffectNode& node,
                                     NodeRuntime& runtime, Effects::CustomComputeBridgeEffect* bridge,
                                     uint32_t firstElement, const float* float4s, uint32_t elementCount);
        // Applies the CommitFields recorded since the last Normal pass.
        void ApplyHostCommits(Graph::GraphModel& graph);
        // Wakes the producer's consumers bound to one of `fields`, whole.
        void WakeFieldConsumers(Graph::GraphModel& graph, uint32_t producerId,
                                const std::unordered_set<std::wstring>& fields);
        // The record of `fields` moved to what their texels already hold:
        // consumers reading the texture take the value without re-rendering;
        // any reading it on the CPU is woken.
        void SyncFieldConsumers(Graph::GraphModel& graph, uint32_t producerId,
                                const std::unordered_set<std::wstring>& fields);
        // A commit changed the producer's committed output.
        void CountCommit(uint32_t producerId);

        // ---- per-node state ----------------------------------------------
        NodeRuntime* FindRuntime(uint32_t nodeId);
        const NodeRuntime* FindRuntime(uint32_t nodeId) const;
        // The compute bridge impl of a D3D11 compute node, or nullptr.
        Effects::CustomComputeBridgeEffect* BridgeFor(uint32_t nodeId) const;
        // The mode the node's last build gave gpu-bindable parameter `gpuBindableIndex`.
        Effects::GpuBindingMode AppliedBindingMode(uint32_t nodeId, uint32_t gpuBindableIndex) const;
        // Once per Normal pass: polls variant compiles and drops the state of
        // nodes no longer in the graph.
        void PollVariantsAndPrune(Graph::GraphModel& graph);

        // ---- the walk (EvaluatorCore.cpp) --------------------------------
        void ApplyFrameOptions(const EvaluatorOptions& previous, Graph::GraphModel& graph);
        // True when a node a binding of `node` reads changed earlier this pass.
        bool BoundSourceChanged(const Graph::EffectNode& node) const;
        // Records that `region` of the node's output changed this pass,
        // counting the node once per pass, and adds it to the pending region.
        void MarkOutputChanged(uint32_t nodeId, const Graph::DirtyRegion& region = Graph::DirtyRegion::Whole());
        // A D3D11 compute node changed: its image output changed whole. An
        // analysis-only node's change is counted but, under the change gate,
        // wakes nothing: PublishFieldsIfChanged does that after its dispatch.
        void MarkComputeChanged(const Graph::EffectNode& node);
        void VisitSource(Graph::GraphModel& graph, Graph::EffectNode& node);
        void VisitBuiltIn(Graph::GraphModel& graph, Graph::EffectNode& node, ID2D1DeviceContext5* dc);
        void VisitParameterNode(Graph::GraphModel& graph, Graph::EffectNode& node);
        ID2D1Image* VisitOutput(Graph::GraphModel& graph, Graph::EffectNode& node);
        // Takes an owning ref to the effect's output and points cachedOutput at it.
        void PublishOutput(Graph::EffectNode& node, ID2D1Effect* effect);
        void DropOutput(Graph::EffectNode& node);

        // ---- custom D2D shader nodes (CustomEffectNode.cpp) --------------
        void VisitCustomEffect(Graph::GraphModel& graph, Graph::EffectNode& node, ID2D1DeviceContext5* dc);
        // Compiles a missing baseline; false when the node cannot render this visit.
        bool EnsurePixelBaseline(Graph::EffectNode& node);
        // OutputW/OutputH and ImageAW..ImageDH from the inputs' bounds; true
        // when a value changed.
        bool InjectHostDimensions(Graph::EffectNode& node, PropertyMap& effectiveProps,
                                  const Graph::GraphModel& graph, ID2D1DeviceContext5* dc);
        // Picks the build, fills the reserved pins, loads the shader and packs the cbuffer.
        void ApplyCustomEffect(ID2D1Effect* effect, Graph::EffectNode& node, const PropertyMap& effectiveProps,
                               const Graph::GraphModel& graph, ID2D1DeviceContext5* dc);

        // ---- D3D11 compute (ComputeScheduler.cpp) ------------------------
        void QueueD3D11Compute(Graph::GraphModel& graph, Graph::EffectNode& node, ID2D1DeviceContext5* dc);
        // Nodes whose dispatch this frame must copy analysis values to the CPU;
        // also asks producers for the lane-3 textures consumers can use.
        std::unordered_set<uint32_t> PlanReadbacks(const Graph::GraphModel& graph);
        // Adds to `cpu` the producers `consumer`'s bindings read on the CPU
        // this frame, and to `gpu` those a pixel shader is expected to read
        // on its texture build. `ready`: the producers that will have
        // published a texture when the consumer's build is chosen.
        void CollectAnalysisDemand(const Graph::EffectNode& consumer, const Graph::GraphModel& graph,
                                   const std::function<bool(uint32_t producerId)>& ready,
                                   std::unordered_set<uint32_t>& cpu, std::unordered_set<uint32_t>* gpu);
        // The GPU-binding bits a pixel shader's build will apply for `plan`;
        // 0 when no build keeping that routing is ready.
        uint32_t ExpectedTextureBits(const Graph::EffectNode& consumer, const Graph::GraphModel& graph,
                                     const GpuBindingPlan& plan);
        // Reads back a producer whose results are on the GPU only, when a
        // host hint or a binding needs them on the CPU.
        void ReadBackIfCpuNeeded(Graph::GraphModel& graph, Graph::EffectNode& producer,
                                 const std::function<bool(uint32_t producerId)>& ready);
        // A build chosen without the GPU binding the plan wanted.
        void NoteFallbackBuild(const NodeVariantState& state);
        // An FP32 bitmap of `image` a bridge can bind, shared by every
        // consumer this pass. nullptr with *failHr on failure.
        ID2D1Bitmap1* PreRenderInput(ID2D1DeviceContext5* dc, ID2D1Image* image,
                                     std::optional<std::pair<uint32_t, uint32_t>> producerPin, HRESULT* failHr);
        // False when no dispatch ran (compile pending, error).
        bool DispatchViaBridge(ID2D1DeviceContext5* dc, const Graph::GraphModel& graph, Graph::EffectNode& node,
                               const std::vector<ID2D1Image*>& inputImages,
                               const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
                               Effects::CustomComputeBridgeEffect* bridge, bool readbackToCpu,
                               std::vector<float>* readbackFloats = nullptr);
        // The image-output size: the effect's hook, then the properties, then input 0.
        D2D1_SIZE_U ComputeImageOutputSize(const Graph::EffectNode& node, const Graph::GraphModel& graph,
                                           const std::vector<ID2D1Image*>& inputImages,
                                           const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
                                           ID2D1DeviceContext5* dc) const;

        // ---- effect creation and wiring (EffectFactory.cpp) --------------
        // Create or retrieve the node's D2D effect. On failure returns nullptr
        // and, when failureReason is given, says why.
        ID2D1Effect* GetOrCreateEffect(ID2D1DeviceContext5* dc, const Graph::EffectNode& node,
                                       std::wstring* failureReason = nullptr);
        static void ApplyProperties(ID2D1Effect* effect, const PropertyMap& effectiveProps);
        // Sets each input pin from its upstream node's cachedOutput.
        void WireInputs(ID2D1Effect* effect, const Graph::EffectNode& destNode, const Graph::GraphModel& graph);
        // 1x1 zero placeholder for reserved, lookup and variadic pins with
        // nothing attached. Direct2D will NOT render a custom effect that has
        // an unconnected input -- it produces an empty output rect, with no
        // error anywhere -- so such a pin cannot be left null. Excluded from
        // output bounds by CustomPixelShaderEffect::ImageInputCount.
        void EnsureAnalysisDummy(ID2D1DeviceContext5* dc);
        // Input for zero-input generators: D2D custom pixel shaders need an
        // input for sizing even when they draw their own content.
        void EnsureDummySourceBitmap(ID2D1DeviceContext5* dc);

        // ---- analysis readback (AnalysisReadback.cpp) --------------------
        void ReadHistogramOutput(ID2D1DeviceContext5* dc, ID2D1Effect* effect, Graph::EffectNode& node);
        void ReadCustomAnalysisOutput(ID2D1DeviceContext5* dc, Graph::EffectNode& node);
        static void UnpackAnalysisFloats(Graph::EffectNode& node, const std::vector<float>& floats);
        // Marks every node bound to `producerId`'s analysis fields dirty,
        // plus everything downstream of those consumers by edge.
        static void MarkBindingConsumersDirty(Graph::GraphModel& graph, uint32_t producerId);
        // The analysis change gate: wakes the producer's bound consumers
        // whose fields changed. `inputRegion` is what a dispatch consumed
        // (null for values landing later, which are judged by value alone);
        // `current` says the producer's fields hold this dispatch's values;
        // `previous` is what its consumers last rendered with, when known.
        // Anything it cannot judge wakes every consumer, as the gate off does.
        void PublishFieldsIfChanged(Graph::GraphModel& graph, const Graph::EffectNode& producer,
                                    const Graph::DirtyRegion* inputRegion, bool current,
                                    const std::vector<Graph::AnalysisFieldValue>* previous);
        // The producer's lane-3 texture as a D2D bitmap, reusing the view
        // while the texture is unchanged; nullptr before its first fill.
        ID2D1Bitmap1* EnsureAnalysisBitmap(uint32_t producerNodeId, ID2D1DeviceContext5* dc);
        // The lane-3 bitmap a producer published, or nullptr.
        ID2D1Bitmap1* AnalysisBitmap(uint32_t producerNodeId) const;

        // ---- output caching (CachePolicy.cpp) ----------------------------
        // Clean-subgraph output caching (D2D1_PROPERTY_CACHED): enabled on
        // every effect-backed node so pulling the terminal image does not
        // re-execute an unchanged subtree. D2D invalidates on the property
        // and topology changes it can see; the two hidden channels --
        // custom-effect cbuffer uploads and in-place texture updates -- arrive
        // here as contentDirty.
        void UpdateEffectCachePolicy(ID2D1Effect* effect, NodeRuntime& runtime, bool contentDirty);

        // ---- state ---------------------------------------------------------
        std::unordered_map<uint32_t, NodeRuntime> m_nodes;

        // Per Evaluate pass, reset at its start.
        struct PassState
        {
            // Nodes whose output changed this pass, and where, so a node
            // visited later can tell whether anything it reads moved (pulled
            // at visit time).
            std::unordered_map<uint32_t, Graph::DirtyRegion> outputChanged;
            // Nodes whose analysis fields moved while their image did not (a
            // video whose bound Time moved). Wakes binding consumers only.
            std::unordered_set<uint32_t> fieldsChanged;
            // Effects created this pass: D2D needs one more render cycle before
            // their output is valid, so they are re-dirtied at its end and
            // their analysis readback waits.
            std::unordered_set<uint32_t> justCreated;
            // Nodes whose outputChangeCount this pass has counted.
            std::unordered_set<uint32_t> counted;
        } m_pass;

        // D3D11 compute dispatches queued by the Evaluate passes of one frame,
        // drained by ProcessDeferredCompute.
        struct DeferredCompute
        {
            uint32_t nodeId{ 0 };
            // One image per input slot (t0..tN-1), in inputNames order. Owning:
            // a later pass in the same frame can rebuild an upstream effect and
            // release the image it recorded.
            std::vector<winrt::com_ptr<ID2D1Image>> inputImages;
        };
        struct ComputeQueue
        {
            std::vector<DeferredCompute> entries;
            // Queued this frame; a compute consumer of one of them must
            // dispatch too, since it reads that producer's analysis SRV.
            std::unordered_set<uint32_t> queued;
            // Producers some pixel-shader consumer wants a lane-3 texture from.
            std::unordered_set<uint32_t> textureWanted;
            // Producers that skip their readback only because a pixel-shader
            // consumer is expected to read them on the GPU this frame.
            std::unordered_set<uint32_t> skipExpected;
            // Producers a binding reads on the CPU this frame, whatever the
            // skip setting: their readbacks are AnalysisReadbackStats::fallbackReadbacks.
            std::unordered_set<uint32_t> cpuDemand;
            void Clear()
            {
                entries.clear(); queued.clear(); textureWanted.clear(); skipExpected.clear(); cpuDemand.clear();
            }
        } m_computeQueue;

        // True for the duration of an EvalMode::Frozen pass.
        bool m_frozen{ false };
        uint32_t m_dispatchesLastFrame{ 0 };
        uint64_t m_cacheInvalidations{ 0 };
        RegionDrawStats m_regionStats;
        AnalysisReadbackStats m_readbackStats;
        BindingGateStats m_bindingGate;
        ReduceCellStats m_reduceCells;
        PreRenderStats m_preRenders;
        DeferredCommitStats m_deferredStats;
        // Frames, for the landing distance: one starts at the first Normal
        // Evaluate after ProcessDeferredCompute closed the last.
        uint64_t m_frame{ 0 };
        bool m_frameOpen{ false };

        // Host hint: nodes whose analysis values must reach the CPU (the
        // selected node, MCP targets). Throttled per node by
        // NodeRuntime::lastHintReadback.
        std::unordered_set<uint32_t> m_cpuAnalysisInterest;
        std::unordered_set<uint32_t> m_uncachedNodes;

        VariantSelector m_variants;
        GpuTimer* m_gpuTimer{ nullptr };
        const Effects::IEffectHooks* m_effectHooks;

        // Device-dependent scratch, released by ReleaseCache.
        winrt::com_ptr<ID2D1Bitmap1> m_analysisDummyBitmap;
        winrt::com_ptr<ID2D1Bitmap1> m_dummySourceBitmap;
        // Pre-render target for a compute input no edge feeds (a placeholder).
        PreRenderTarget m_unwiredPreRender;
        // Temp targets for forcing an analysis effect to compute. Separate per
        // reader: the histogram wants BGRA and the custom readback FP32.
        winrt::com_ptr<ID2D1Bitmap1> m_histogramTarget;
        uint32_t m_histogramTargetW{ 0 };
        uint32_t m_histogramTargetH{ 0 };
        winrt::com_ptr<ID2D1Bitmap1> m_analysisTarget;
        uint32_t m_analysisTargetW{ 0 };
        uint32_t m_analysisTargetH{ 0 };
        winrt::com_ptr<ID2D1Bitmap1> m_analysisCpuBitmap;
        uint32_t m_analysisCpuBitmapW{ 0 };

        // Options: pinned by SetOptions (else the process defaults), copied
        // into m_frameOptions at the start of each Normal Evaluate. Until the
        // first frame m_frameOptions holds the built-in defaults.
        mutable std::mutex m_optionsMutex;
        std::optional<EvaluatorOptions> m_pinnedOptions;
        EvaluatorOptions m_frameOptions;
    };
}
