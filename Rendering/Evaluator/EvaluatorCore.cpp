#include "pch_engine.h"
#include "EvaluatorCore.h"
#include "ParameterNodes.h"
#include "../../Effects/Performance.h"
#include "../../Graph/SourceKind.h"

using namespace ShaderLab::Graph;

namespace ShaderLab::Rendering::Evaluator
{
    namespace
    {
        constexpr std::wstring_view cCycleError =
            L"In a dependency cycle (image edges or property bindings), or fed by one; the graph cannot render.";

        // A video Source: its image comes from SourceNodeFactory's uploads,
        // and its bound properties (Time) only steer the provider.
        bool IsVideoSource(const EffectNode& node)
        {
            return ClassifySource(node) == SourceKind::Video;
        }

        bool IsD3D11Compute(const EffectNode& node)
        {
            return node.customEffect.has_value() &&
                   node.customEffect->shaderType == CustomShaderType::D3D11ComputeShader;
        }
    }

    EvaluatorCore::EvaluatorCore()
        : m_effectHooks(&Effects::BuiltInEffectHooks())
    {
    }

    // -----------------------------------------------------------------------
    // Options
    // -----------------------------------------------------------------------

    void EvaluatorCore::SetOptions(const EvaluatorOptions& options)
    {
        std::lock_guard lock(m_optionsMutex);
        m_pinnedOptions = options;
    }

    void EvaluatorCore::FollowDefaultOptions()
    {
        std::lock_guard lock(m_optionsMutex);
        m_pinnedOptions.reset();
    }

    EvaluatorOptions EvaluatorCore::Options() const
    {
        {
            std::lock_guard lock(m_optionsMutex);
            if (m_pinnedOptions) return *m_pinnedOptions;
        }
        return Performance::DefaultEvaluatorOptions();
    }

    void EvaluatorCore::ApplyFrameOptions(const EvaluatorOptions& previous)
    {
        if (previous.subRectInputDemand == m_frameOptions.subRectInputDemand)
            return;
        for (auto& [nodeId, runtime] : m_nodes)
            if (runtime.instance.pixelImpl)
                runtime.instance.pixelImpl->SetSubRectInputDemand(m_frameOptions.subRectInputDemand);
    }

    // -----------------------------------------------------------------------
    // Per-node state
    // -----------------------------------------------------------------------

    NodeRuntime* EvaluatorCore::FindRuntime(uint32_t nodeId)
    {
        auto it = m_nodes.find(nodeId);
        return it == m_nodes.end() ? nullptr : &it->second;
    }

    const NodeRuntime* EvaluatorCore::FindRuntime(uint32_t nodeId) const
    {
        auto it = m_nodes.find(nodeId);
        return it == m_nodes.end() ? nullptr : &it->second;
    }

    Effects::CustomComputeBridgeEffect* EvaluatorCore::BridgeFor(uint32_t nodeId) const
    {
        const auto* runtime = FindRuntime(nodeId);
        return runtime ? runtime->instance.bridgeImpl : nullptr;
    }

    winrt::com_ptr<ID3D11ShaderResourceView> EvaluatorCore::AnalysisBuffer(uint32_t nodeId) const
    {
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
        winrt::com_ptr<Effects::IEngineComputeOutput> output;
        // The bridge impl, not the D2D effect: D2D's outer QueryInterface
        // does not reach the impl's interfaces.
        if (auto* bridge = BridgeFor(nodeId);
            bridge && SUCCEEDED(bridge->QueryInterface(__uuidof(Effects::IEngineComputeOutput), output.put_void())))
            output->GetAnalysisSrv(srv.put());
        return srv;
    }

    HRESULT EvaluatorCore::WriteAnalysis(uint32_t nodeId, uint32_t firstElement, const float* float4s, uint32_t elementCount)
    {
        auto* bridge = BridgeFor(nodeId);
        return bridge ? bridge->WriteResults(firstElement, float4s, elementCount) : E_NOT_VALID_STATE;
    }

    uint32_t EvaluatorCore::AppliedGpuBits(uint32_t nodeId) const
    {
        const auto* runtime = FindRuntime(nodeId);
        return runtime ? runtime->variant.appliedGpuBits : 0u;
    }

    Effects::GpuBindingMode EvaluatorCore::AppliedBindingMode(uint32_t nodeId, uint32_t gpuBindableIndex) const
    {
        return Effects::BindingModeAt(AppliedGpuBits(nodeId), gpuBindableIndex);
    }

    uint64_t EvaluatorCore::OutputChangeCount(uint32_t nodeId) const
    {
        const auto* runtime = FindRuntime(nodeId);
        return runtime ? runtime->outputChangeCount : 0;
    }

    void EvaluatorCore::PollVariantsAndPrune(GraphModel& graph)
    {
        for (auto it = m_nodes.begin(); it != m_nodes.end();)
        {
            auto* node = graph.FindNode(it->first);
            if (!node)
            {
                it = m_nodes.erase(it);
                continue;
            }
            VariantSelector::Poll(*node, it->second.variant);
            ++it;
        }
    }

    void EvaluatorCore::MarkOutputChanged(uint32_t nodeId)
    {
        if (m_pass.outputChanged.insert(nodeId).second)
            ++m_nodes[nodeId].outputChangeCount;
    }

    bool EvaluatorCore::BoundSourceChanged(const EffectNode& node) const
    {
        return AnyBindingSource(node, [this](uint32_t sourceNodeId)
        {
            return m_pass.outputChanged.count(sourceNodeId) > 0 ||
                   m_pass.fieldsChanged.count(sourceNodeId) > 0;
        });
    }

    void EvaluatorCore::PublishOutput(EffectNode& node, ID2D1Effect* effect)
    {
        winrt::com_ptr<ID2D1Image> output;
        effect->GetOutput(output.put());
        m_nodes[node.id].instance.output = output;
        node.cachedOutput = output.get();
    }

    void EvaluatorCore::DropOutput(EffectNode& node)
    {
        node.cachedOutput = nullptr;
        if (auto* runtime = FindRuntime(node.id))
            runtime->instance.output = nullptr;
    }

    // -----------------------------------------------------------------------
    // The walk
    // -----------------------------------------------------------------------

    ID2D1Image* EvaluatorCore::Evaluate(GraphModel& graph, ID2D1DeviceContext5* dc, EvalMode mode)
    {
        if (graph.IsEmpty() || !dc)
            return nullptr;

        const bool frozen = mode == EvalMode::Frozen;
        struct FrozenScope
        {
            bool& flag;
            bool saved;
            ~FrozenScope() { flag = saved; }
        } frozenScope{ m_frozen, m_frozen };
        m_frozen = frozen;

        // Effects created on the previous pass are now fully initialized.
        m_pass = {};

        // A frame's options are fixed by its Normal pass; a Frozen sweep reuses them.
        if (!frozen)
        {
            const EvaluatorOptions previous = m_frameOptions;
            m_frameOptions = Options();
            m_variants.SetCompilePrebuilt(m_frameOptions.compilePrebuiltEffects);
            ApplyFrameOptions(previous);
            // Re-dirty nodes whose wanted variant finished compiling.
            PollVariantsAndPrune(graph);
        }

        // The compute queue is NOT cleared here: its entries hold owning image
        // refs, and a later pass of the same frame finds a node it queued
        // already clean and does not queue it again. ProcessDeferredCompute
        // drains it.

        std::vector<uint32_t> order;
        try
        {
            order = graph.TopologicalSort();
        }
        catch (const std::logic_error&)
        {
            // No order exists; name the nodes that a cycle holds back.
            for (uint32_t nodeId : graph.UnorderableNodes())
                if (auto* node = graph.FindNode(nodeId))
                    node->runtimeError = std::wstring(cCycleError);
            return nullptr;
        }
        for (auto& node : graph.MutableNodes())
            if (node.runtimeError == cCycleError) node.runtimeError.clear();

        // Async readbacks that landed since the last pass mark their bound
        // consumers dirty, so this runs before the walk.
        if (!frozen && HasPendingReadbacks())
            PollAsyncReadbacks(graph);

        // Dirty propagation is PULL-based, done at each node's visit. A push
        // along edges up front misses property bindings (a Clock has no
        // edges, only bindings) and any node that becomes dirty mid-walk when
        // a bound value moves, and either leaves an analysis node downstream
        // of animated content serving stale values. Pulling is correct by
        // construction: in topological order every predecessor, edge or
        // binding, has finished its visit.
        ID2D1Image* finalOutput = nullptr;
        for (uint32_t nodeId : order)
        {
            EffectNode* node = graph.FindNode(nodeId);
            if (!node)
                continue;

            // Did anything this node reads change earlier in the pass?
            if (!node->dirty)
            {
                for (const auto* edge : graph.GetInputEdges(nodeId))
                {
                    if (m_pass.outputChanged.count(edge->sourceNodeId))
                    { node->dirty = true; break; }
                }
            }
            if (!node->dirty && BoundSourceChanged(*node))
            {
                // A video's bound properties drive its provider, which marks
                // the node dirty itself when a new frame is uploaded. Until
                // then only its analysis fields move.
                if (IsVideoSource(*node))
                    m_pass.fieldsChanged.insert(nodeId);
                else
                    node->dirty = true;
            }

            // Record BEFORE the visit for the start-of-frame case; the binding
            // sites add themselves when they discover a change mid-visit.
            if (node->dirty)
                MarkOutputChanged(nodeId);

            // A node waits on its baseline only while it has none and is rendered.
            if (node->compilePending &&
                (!node->needed || !node->customEffect.has_value() || node->customEffect->isCompiled()))
                node->compilePending = false;

            // Skip nodes no visible output needs -- but KEEP their pending
            // change, so switching the preview to one shows it current.
            // GraphModel::HasDirtyNodes ignores unneeded nodes, so a parked
            // change does not keep the host rendering.
            if (!node->needed)
                continue;

            switch (node->type)
            {
            case NodeType::Source:
                VisitSource(graph, *node);
                break;
            case NodeType::BuiltInEffect:
                VisitBuiltIn(graph, *node, dc);
                break;
            case NodeType::PixelShader:
            case NodeType::ComputeShader:
                if (IsD3D11Compute(*node))
                    QueueD3D11Compute(graph, *node, dc);
                else if (node->customEffect.has_value() && IsParameterKind(node->customEffect->nodeKind))
                    VisitParameterNode(graph, *node);
                else
                    VisitCustomEffect(graph, *node, dc);
                break;
            case NodeType::Output:
                if (auto* image = VisitOutput(graph, *node))
                    finalOutput = image;
                break;
            }
        }

        // D2D needs one full render cycle to initialize a new effect's
        // transform pipeline before its output is valid.
        for (uint32_t id : m_pass.justCreated)
            if (auto* node = graph.FindNode(id)) node->dirty = true;

        return finalOutput;
    }

    void EvaluatorCore::VisitSource(GraphModel& graph, EffectNode& node)
    {
        // A Source's cachedOutput is set by its provider (image, video,
        // capture). Bindings only steer the provider (Clock -> Video.Time).
        if (!node.propertyBindings.empty())
        {
            PropertyMap effectiveProps;
            if (ResolveAndWriteBackBindings(node, graph, effectiveProps))
            {
                if (IsVideoSource(node))
                {
                    m_pass.fieldsChanged.insert(node.id);
                }
                else
                {
                    node.dirty = true;
                    MarkOutputChanged(node.id);
                }
            }
        }
        node.dirty = false;
    }

    void EvaluatorCore::VisitBuiltIn(GraphModel& graph, EffectNode& node, ID2D1DeviceContext5* dc)
    {
        std::wstring createError;
        ID2D1Effect* effect = GetOrCreateEffect(dc, node, &createError);
        // Creation is retried on every visit whatever `dirty` says, so a
        // failure must not leave the flag set: a node stuck dirty keeps
        // HasDirtyNodes() true and costs every frame a second full Evaluate
        // plus the frozen post-dispatch pass.
        if (!effect)
        {
            node.dirty = false;
            // An existing error (a bad effectClsid at load) names the cause better.
            if (node.runtimeError.empty())
                node.runtimeError = std::wstring(cCreateEffectFailedPrefix) + createError;
            return;
        }
        if (node.runtimeError.starts_with(cCreateEffectFailedPrefix))
            node.runtimeError.clear();

        WireInputs(effect, node, graph);

        PropertyMap effectiveProps;
        const bool bindingsChanged = ResolveAndWriteBackBindings(node, graph, effectiveProps);
        const bool wasDirty = node.dirty || bindingsChanged;
        // A binding that moved changes this node's output, so downstream
        // consumers have to learn about it too.
        if (bindingsChanged) MarkOutputChanged(node.id);
        if (wasDirty)
        {
            ApplyProperties(effect, effectiveProps);
            node.dirty = false;
        }

        // Upstream in-place texture updates arrive as node.dirty via the
        // dirty walks; D2D itself cannot see them.
        UpdateEffectCachePolicy(effect, m_nodes[node.id], wasDirty);
        PublishOutput(node, effect);

        // An analysis effect (Histogram) computes when its output is drawn,
        // so read it back when something changed.
        if (wasDirty && node.effectClsid.has_value() &&
            IsEqualGUID(node.effectClsid.value(), CLSID_D2D1Histogram))
            ReadHistogramOutput(dc, effect, node);
    }

    void EvaluatorCore::VisitParameterNode(GraphModel& graph, EffectNode& node)
    {
        if (!node.propertyBindings.empty())
        {
            PropertyMap effectiveProps;
            ResolveAndWriteBackBindings(node, graph, effectiveProps);
        }
        node.analysisOutput.type = AnalysisOutputType::Typed;
        node.analysisOutput.fields = ParameterNodeFields(node);
        node.cachedOutput = nullptr;
        node.dirty = false;
    }

    ID2D1Image* EvaluatorCore::VisitOutput(GraphModel& graph, EffectNode& node)
    {
        // The output node passes its first input through.
        node.cachedOutput = nullptr;
        auto inputs = graph.GetInputEdges(node.id);
        if (!inputs.empty())
        {
            const EffectNode* srcNode = graph.FindNode(inputs[0]->sourceNodeId);
            if (srcNode && srcNode->cachedOutput)
                node.cachedOutput = srcNode->cachedOutput;
        }
        node.dirty = false;
        return node.cachedOutput;
    }

    void EvaluatorCore::ResolveSourceBindings(GraphModel& graph)
    {
        for (auto& node : graph.MutableNodes())
        {
            if (node.type != NodeType::Source || node.propertyBindings.empty()) continue;

            PropertyMap effectiveProps;
            if (!ResolveAndWriteBackBindings(node, graph, effectiveProps)) continue;
            // A video's image changes only when TickAndUploadVideos uploads a frame.
            if (!IsVideoSource(node))
                node.dirty = true;
        }
    }
}
