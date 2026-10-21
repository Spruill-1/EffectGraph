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

        void SetCpuAnalysisInterest(std::unordered_set<uint32_t> ids);
        void ClearCpuAnalysisInterest();
        void RequestFreshCpuReadback(uint32_t nodeId) { m_nodes[nodeId].freshReadbackRequested = true; }
        uint32_t AppliedGpuBits(uint32_t nodeId) const;
        winrt::com_ptr<ID3D11ShaderResourceView> AnalysisBuffer(uint32_t nodeId) const;
        HRESULT WriteAnalysis(uint32_t nodeId, uint32_t firstElement, const float* float4s, uint32_t elementCount);

    private:
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
        void ApplyFrameOptions(const EvaluatorOptions& previous);
        // True when a node a binding of `node` reads changed earlier this pass.
        bool BoundSourceChanged(const Graph::EffectNode& node) const;
        // Records that the node's output changed this pass, counting it once per pass.
        void MarkOutputChanged(uint32_t nodeId);
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
        // An FP32 bitmap of `image` a bridge can bind, shared by every
        // consumer this pass. nullptr with *failHr on failure.
        ID2D1Bitmap1* PreRenderInput(ID2D1DeviceContext5* dc, ID2D1Image* image,
                                     std::optional<std::pair<uint32_t, uint32_t>> producerPin, HRESULT* failHr);
        // False when no dispatch ran (compile pending, error).
        bool DispatchViaBridge(ID2D1DeviceContext5* dc, const Graph::GraphModel& graph, Graph::EffectNode& node,
                               const std::vector<ID2D1Image*>& inputImages,
                               const std::vector<ID2D1Bitmap1*>& preRenderedInputs,
                               Effects::CustomComputeBridgeEffect* bridge, bool readbackToCpu);
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
            // Nodes whose output changed this pass, so a node visited later
            // can tell whether anything it reads moved (pulled at visit time).
            std::unordered_set<uint32_t> outputChanged;
            // Nodes whose analysis fields moved while their image did not (a
            // video whose bound Time moved). Wakes binding consumers only.
            std::unordered_set<uint32_t> fieldsChanged;
            // Effects created this pass: D2D needs one more render cycle before
            // their output is valid, so they are re-dirtied at its end and
            // their analysis readback waits.
            std::unordered_set<uint32_t> justCreated;
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
            void Clear() { entries.clear(); queued.clear(); textureWanted.clear(); skipExpected.clear(); }
        } m_computeQueue;

        // True for the duration of an EvalMode::Frozen pass.
        bool m_frozen{ false };
        uint32_t m_dispatchesLastFrame{ 0 };
        uint64_t m_cacheInvalidations{ 0 };

        // Host hint: nodes whose analysis values must reach the CPU (the
        // selected node, MCP targets). Throttled per node by
        // NodeRuntime::lastHintReadback.
        std::unordered_set<uint32_t> m_cpuAnalysisInterest;

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
